// SPDX-License-Identifier: Apache-2.0

// fixtures.h の実装。場面ごとに Observation を組む (View は ledger::decide_dock() と stabilize() が作る)。
// 時刻は 2000-01-01 13:43:00 (日本時間) に固定し、時差は +9 時間にする (同じ入力から同じ画像を作るため)。場面は、
// 問題のない 1 つの状態 (connected_in_call) から 1 か所ずつ変えて作る (tests/test_ledger_rules.cpp の healthy() と
// 同じ作り方)。変えた所が場面の名前になる。名前と ID は架空のもので、ID は 18 桁 (実際の ID と同じ桁数) の
// 1000...0NN。設定から決まる Observation のフィールド (自動配置・メインアカウント・録画の保存先など) は場面の
// settings に書き、製品と同じく observation_for が設定から Observation へコピーする
// (dock/dock_poll.cpp の current_facts と同じ向き。同じ値を 2 か所に書かない)。

#include "fixtures.h"

#include "ledger/notice_tracker.h"
#include "ledger/rules.h"
#include "ledger/status_tab.h"
#include "ledger/text_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cordmux::ui_preview {

namespace {

using namespace cordmux::ledger;
namespace keys = cordmux::ledger::text::keys;

constexpr std::int64_t kSec = 1'000'000;
// 2000-01-01T04:43:00Z (= 13:43:00 JST)。
constexpr std::int64_t kNow = 946'701'780LL * kSec;
constexpr int kJst = 9 * 3600;

// 架空の ID (18 桁)。
std::string fake_id(int n)
{
	std::string digits = std::to_string(n);
	return std::string(18 - digits.size() - 1, '0').insert(0, "1") + digits;
}

// 架空の名前 (幅の見え方を確認するためのセット: 短い英字、空白を含む名前、絵文字を含む名前、長い日本語、区切りのない
// 長い英字)。
const char *const kNameHanako = "hanako";
const char *const kNameTaro = "Taro Yamada";
const char *const kNameSakura = "さくら🌸";
const char *const kNameKenji = "kenji";
const char *const kNameLongJa = "とても長い表示名の人がいるときの例です";
const char *const kNameLongEn = "AVeryLongDisplayNameWithoutAnySpaces_2026";
const char *const kNameMika = "Mika ✨ 配信中";
const char *const kNameRen = "ren";
const char *const kNameReceiver = "cordmux-receiver";

// アンインストールの後に削除するフォルダー。製品は、DLL のフォルダーが「<...>/plugins/<名前>」の形のときだけ、その
// フォルダーを返す (install/uninstall.h)。ここでは OBS の既定のプラグインのフォルダー (ProgramData の plugins) の形にする
// (<名前>/bin/64bit の形では、製品は空を返す。手順書と違う置き方の場面がこれに当たる)。
const char *const kFolderToDelete = "C:\\ProgramData\\obs-studio\\plugins\\cordmux";

// アンインストールが段階 (3) で止まったときに、バックアップ (Vesktop の設定を元に戻すための値) を残した cordmux の
// 設定のフォルダー (OBS の plugin_config の cordmux。ledger::UninstallFacts::settings_dir)。
const char *const kSettingsDirKept = "C:\\Users\\user\\AppData\\Roaming\\obs-studio\\plugin_config\\cordmux";

// 普通の長さの場所。Windows のユーザー名の上限 (20 文字) の架空の名前の下に置く。OBS の既定の録画の保存先は利用者の
// 「ビデオ」(OBS 31 の frontend/utility/platform-windows.cpp の CSIDL_MYVIDEO)、診断情報の保存のダイアログの既定値は
// 「デスクトップ」。
const char *const kUsualRecordDir = "C:\\Users\\example_streamer_001\\Videos\\cordmux";
const char *const kUsualDesktopZip = "C:\\Users\\example_streamer_001\\Desktop\\cordmux-diagnostics-20000101-134300.zip";

PersonFact showing(int n, const char *name, int slot, int width = 1280, int height = 720, double fps = 30.0)
{
	PersonFact person;
	person.user_id = fake_id(n);
	person.display_name = std::string(name);
	// 送信側の profiles が送ってくる Discord での名前 (名前を変更していない人は表示名と同じ。build_observation が
	// コピーする形)。Discord での名前が同じ 2 人を 2 行目のユーザー名で見分ける規則は、このフィールドで数える。
	person.discord_name = std::string(name);
	person.slot = slot;
	person.last_video_frame_at_us = kNow - kSec / 2;
	person.width = width;
	person.height = height;
	person.fps = fps;
	person.stage = std::string(kStageNative);
	person.sharing = true;
	// 送信側の facts が届いていて、画面共有していると分かっている (分かっていない間は、画面共有のアイコンを
	// 表示しない)。
	person.sharing_known = true;
	person.sharing_since_us = kNow - 60 * kSec;
	person.in_roster = true;
	person.voice_arriving = true;
	return person;
}

PersonFact listening(int n, const char *name)
{
	PersonFact person;
	person.user_id = fake_id(n);
	person.display_name = std::string(name);
	person.discord_name = std::string(name); // showing() のコメント
	person.voice_arriving = true;
	person.in_roster = true;
	// 送信側の facts が届いていて、画面共有していないと分かっている (「画面共有していない」と表示できる
	// 場面にする)。
	person.sharing_known = true;
	return person;
}

PersonFact receiver()
{
	PersonFact person = listening(90, kNameReceiver);
	person.is_receiver = true;
	return person;
}

SeenPerson seen(int n, const char *name, std::int64_t ago_us)
{
	SeenPerson person;
	person.user_id = fake_id(n);
	person.display_name = std::string(name);
	person.last_seen_at_us = kNow - ago_us;
	return person;
}

void fill_versions(Observation &obs)
{
	obs.versions.plugin = "0.1.0+dev";
	obs.versions.discord_side = "0.1.0+dev";
	obs.versions.vesktop = "1.5.8";
	obs.versions.bundled_vencord = "0.1.0+dev";
	obs.versions.bundled_vencord_commit = "4f2c9a1";
	obs.versions.bundled_vencord_tag = "v1.15.9"; // マーカーファイルに書いた Vencord のバージョン
	obs.versions.obs = "31.1.1";
	obs.versions.windows = "10.0.12345.6789";
	obs.versions.gpu = "Example GPU";
}

// 接続していて、通話に 4 人いて、3 人を枠に載せている (配信中のいちばん普通の状態)。
Observation connected_in_call()
{
	Observation obs;
	obs.now_us = kNow;
	obs.utc_offset_sec = kJst;
	obs.choices.feature = FeatureSwitch::On;
	obs.choices.recording = FeatureSwitch::Off;
	obs.choices.dummy = FeatureSwitch::Off;
	obs.choices.composite_checked = true;
	obs.choices.composite_present = true;
	obs.choices.discord_audio_source = DiscordAudioSource::NotFound;
	// cordmux のソースが配信に出ていて、鳴る。
	obs.choices.composite_active = true;
	obs.choices.composite_audible = true;
	obs.listener.state = ListenerState::Listening;
	obs.install.process = VesktopProcess::Running;
	obs.install.dir = VencordDirState::Matches;
	obs.link.state = LinkState::Connected;
	obs.link.connected_at_us = kNow - 600 * kSec;
	obs.call.presence = Presence::InCall;
	obs.call.roster_count = 4;
	fill_versions(obs);
	obs.uninstall.obs_streaming = false;
	obs.uninstall.obs_recording = false;
	obs.people = {showing(1, kNameHanako, 1), showing(2, kNameTaro, 2, 1920, 1080, 60.0),
		      showing(3, kNameSakura, 3), listening(4, kNameKenji)};
	obs.seen.people = {seen(1, kNameHanako, 0), seen(2, kNameTaro, 0), seen(3, kNameSakura, 0),
			   seen(4, kNameKenji, 0)};
	// 通話に参加した時刻 (cordmux が接続した時点で既に通話にいた人は、その時点の時刻で同じ)。
	for (const PersonFact &person : obs.people)
		obs.seen.call_joined_at_us[person.user_id] = kNow - 600 * kSec;
	return obs;
}

// 接続していない (Vesktop は起動している)。インストールの行が成り立つ場面は、ここから作る。
Observation not_connected()
{
	Observation obs = connected_in_call();
	obs.link.state = LinkState::NeverConnected;
	obs.link.connected_at_us.reset();
	obs.call.presence = Presence::Unknown;
	obs.call.roster_count.reset();
	obs.people.clear();
	obs.versions.discord_side.clear();
	obs.versions.vesktop.clear();
	return obs;
}

// 人数が多い場面 (30 人)。枠は埋まっていて、6 人が画面共有している。参加者タブ、音声タブ、状態タブの 30 人の場面が使う。
Observation thirty_people()
{
	Observation many = connected_in_call();
	many.choices.composite_present = true;
	many.people = {receiver()};
	many.seen.people.clear();
	for (int i = 0; i < 30; ++i) {
		const std::string name = "guest" + std::to_string(i + 1);
		PersonFact person = i < 6 ? showing(100 + i, name.c_str(), i < 4 ? i + 1 : 1) : listening(100 + i, name.c_str());
		if (i >= 4)
			person.slot.reset();
		person.audio.has_screen_audio = i < 6;
		many.people.push_back(person);
		many.seen.people.push_back(seen(100 + i, name.c_str(), 0));
	}
	many.call.roster_count = 30;
	return many;
}

Fixture make(std::string id, std::string note, Observation obs)
{
	Fixture f;
	f.id = std::move(id);
	f.note = std::move(note);
	f.obs = std::move(obs);
	return f;
}

void add_status_fixtures(std::vector<Fixture> &out)
{
	// ---- primary の表のすべての行 ----
	{
		Observation obs;
		obs.now_us = kNow;
		obs.utc_offset_sec = kJst;
		obs.choices.feature = FeatureSwitch::On;
		obs.versions.plugin = "0.1.0+dev";
		obs.versions.bundled_vencord = "0.1.0+dev";
		obs.versions.obs = "31.1.1";
		out.push_back(make("status-starting", "起動した直後 (まだ何も確認していない)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.choices.feature = FeatureSwitch::Off;
		obs.link.state = LinkState::Disconnected;
		obs.listener.state = ListenerState::Closed;
		obs.people.clear();
		obs.call.presence = Presence::Unknown;
		obs.call.roster_count.reset();
		out.push_back(make("status-feature-off", "cordmux を無効にしている (録画のボタンは押せず、理由が表示される)", obs));
	}
	{
		Observation obs = not_connected();
		obs.listener.state = ListenerState::BlockedByOtherWriter;
		obs.listener.other_writer_pid = 4242;
		obs.listener.other_writer_started_at_us = kNow - 3600 * kSec;
		out.push_back(make("status-other-obs", "別の OBS が cordmux を使っている", obs));
	}
	{
		Observation obs = not_connected();
		obs.listener.state = ListenerState::PortsExhausted;
		obs.listener.port_first = 47800;
		obs.listener.port_last = 47815;
		out.push_back(make("status-ports-exhausted", "待ち受けのポートがすべて使われている", obs));
	}
	{
		// 待ち受けは始めたが、接続のための情報のファイルを保存できなかった (ledger/rules.cpp の primary の表の
		// SessionFileNotSaved の行)。
		Observation obs = not_connected();
		obs.listener.state = ListenerState::Listening;
		obs.listener.session_file_not_saved = true;
		out.push_back(make("status-session-file-not-saved", "接続のための情報のファイルを保存できなかった", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.call.presence = Presence::NotInCall;
		obs.call.roster_count = 0;
		obs.people.clear();
		out.push_back(make("status-connected-no-call", "接続しているが、通話に参加していない", obs));
	}
	out.push_back(make("status-connected-in-call", "接続していて、通話に 4 人 (配信中の普通の画面)",
			   connected_in_call()));
	{
		Observation obs = connected_in_call();
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.enabled_since_us = kNow - 120 * kSec;
		obs.recording.sender_recording = true;
		obs.recording.remaining_seconds = 5 * 3600;
		out.push_back(make("status-recording-button", "録画中 (ボタンが [録画を終了] に変わる)", obs));
	}
	{
		Observation obs = not_connected();
		obs.install.dir = VencordDirState::HashMismatch;
		obs.install.mismatched_files = {"vencordDesktopMain.js", "vencordDesktopPreload.js",
						"vencordDesktopRenderer.js", "vencordDesktopRenderer.css"};
		out.push_back(make("status-vencord-overwritten", "送信側の cordmux が上書きされて消えた (4 ファイル)", obs));
	}
	{
		Observation obs = not_connected();
		obs.install.dir = VencordDirState::PointsElsewhere;
		out.push_back(make("status-vencord-elsewhere", "Vesktop の設定が別の場所を指している", obs));
	}
	{
		// Vesktop の設定を書き込まない起動。[Vesktopに追加] を置かず、理由と次の操作を文で伝える。手で
		// 設定を false にした場合 (writes-off)、起動のときに設定を読めなかった場合 (writes-off-unread)、別の
		// 場所を指している場合 (elsewhere-writes-off) の 3 つ。
		Observation obs = not_connected();
		obs.install.dir = VencordDirState::NotConfigured;
		obs.install.vesktop_writes_off = true;
		out.push_back(make("status-vencord-writes-off", "Vesktop に追加されていない、この起動では Vesktop の設定を書かない (手で止めた)", obs));
		obs.ops.settings_unreadable_at_startup = true;
		out.push_back(make("status-vencord-writes-off-unread",
				   "Vesktop に追加されていない、起動のときに cordmux の設定を読めなかったので書かない", obs));
		Observation other = not_connected();
		other.install.dir = VencordDirState::PointsElsewhere;
		other.install.vesktop_writes_off = true;
		out.push_back(make("status-vencord-elsewhere-writes-off", "Vesktop の設定が別の場所を指している、この起動では書かない", other));
	}
	{
		Observation obs = not_connected();
		obs.install.dir = VencordDirState::NotConfigured;
		obs.choices.composite_present = false;
		out.push_back(make("status-first-day", "インストールした日 (Vesktop に追加する前で、cordmux のソースもまだない)", obs));
		// 手順書のとおり Vesktop を終了した後。[Vesktopに追加] を押せる (Vesktop の起動中は、上の場面のとおり押せず、
		// 押せない理由がボタンの下に出る)。
		obs.install.process = VesktopProcess::NotRunning;
		out.push_back(make("status-first-day-vesktop-quit",
				   "インストールした日に Vesktop を終了した ([Vesktopに追加] を押せる)", obs));
	}
	{
		Observation obs = not_connected();
		obs.install.discord_side_written_at_us = kNow - 30 * kSec;
		obs.install.vesktop_started_at_us = kNow - 3600 * kSec;
		out.push_back(make("status-restart-vesktop", "Vesktop の再起動が必要", obs));
	}
	const std::pair<DisconnectReason, const char *> reasons[] = {
		{DisconnectReason::SecretMismatch, "secret"},   {DisconnectReason::WireMismatch, "version"},
		{DisconnectReason::HeartbeatTimeout, "no-reply"}, {DisconnectReason::PeerClosed, "peer-closed"},
		{DisconnectReason::Unknown, "unknown"},
	};
	for (const auto &[reason, name] : reasons) {
		Observation obs = not_connected();
		obs.link.state = LinkState::Disconnected;
		obs.link.connected_at_us = kNow - 900 * kSec;
		obs.link.disconnected_at_us = kNow - 75 * kSec;
		obs.link.reason = reason;
		out.push_back(make(std::string("status-dropped-") + name,
				   std::string("接続した後に切れた (理由: ") + name + ")", obs));
	}
	{
		Observation obs = not_connected();
		obs.install.process = VesktopProcess::Unknown;
		out.push_back(make("status-vesktop-checking", "Vesktop が起動しているか、まだ確認していない", obs));
	}
	{
		Observation obs = not_connected();
		obs.install.process = VesktopProcess::NotRunning;
		out.push_back(make("status-vesktop-not-running", "Vesktop が起動していない", obs));
		// 状態タブの「Vesktop」の組で、[Vesktopから削除] の下に押せない理由が出る唯一の場合 (組は Vesktop が起動して
		// いない間だけあるので、残る理由はこの起動で Vesktop の設定を書かないことだけである)。
		obs.install.vesktop_writes_off = true;
		out.push_back(make("status-vesktop-not-running-writes-off",
				   "Vesktop が起動していない、この起動では Vesktop の設定を書かない ([Vesktopから削除] を押せない)", obs));
	}
	out.push_back(make("status-vesktop-not-linking", "Vesktop は起動しているが、接続してこない",
			   not_connected()));
	// 配信中に Vesktop が終了した。ledger/rules.cpp の primary の表の ConnectedThenDropped の行は、Vesktop が
	// 起動していなくても、配信中で枠に人が載っていれば成り立ち、メッセージのレベル 1 の切断のカードの本文が
	// 「Vesktopが起動していないため、...」になる (ledger/rules_dock.cpp の ConnectedThenDropped)。
	// 配信していなければ、VesktopNotRunning の行の平常の 1 行「Vesktopが起動していません」になる。手順書
	// (guide/ja.md と guide/en.md) の引用 (ja は「「Vesktopとの接続が13:41に切れました」のような
	// メッセージ」、en は "a notice that starts with "Disconnected from Vesktop"") と、描画したテキストを並べて
	// 見るため、切れた時刻は 13:41 (kNow の 75 秒前) にする。切れた後の状態は製品に合わせる。受信側は接続が切れると
	// facts・最後のフレームの時刻・profiles を捨て (pipeline/pipeline.cpp)、枠に載っている人だけが残る
	// (dock/build_observation.cpp の people_of。表示名は seen に覚えた名前)。not_connected() から作らないのは、人が
	// 消えて枠に人が載っている条件が成り立たず、ConnectedThenDropped の行にならないためである。通話のフィールドを
	// 残さないのは、切れた後も通話の人数を表示すると製品と違うためである。
	{
		const auto vesktop_stopped = [](bool streaming, bool recording) {
			Observation obs = connected_in_call();
			obs.obs_outputs.streaming = streaming;
			obs.uninstall.obs_streaming = streaming;
			obs.choices.recording = recording ? FeatureSwitch::On : FeatureSwitch::Off;
			obs.install.process = VesktopProcess::NotRunning;
			obs.link.state = LinkState::Disconnected;
			obs.link.connected_at_us = kNow - 900 * kSec;
			obs.link.disconnected_at_us = kNow - 75 * kSec;
			obs.link.reason = DisconnectReason::PeerClosed;
			obs.call.presence = Presence::Unknown;
			obs.call.roster_count.reset();
			obs.versions.discord_side.clear();
			obs.versions.vesktop.clear();
			// 製品の DockPoll は、接続している間の参加者を覚えていて、切れた後も行を残す。
			obs.last_known_people = obs.people;
			std::vector<PersonFact> kept;
			for (const PersonFact &person : obs.people) {
				if (!person.slot.has_value())
					continue;
				PersonFact slotted;
				slotted.user_id = person.user_id;
				slotted.display_name = person.display_name;
				slotted.slot = person.slot;
				kept.push_back(slotted);
			}
			obs.people = kept;
			return obs;
		};
		out.push_back(make("status-dropped-vesktop-stopped", "配信中に Vesktop が終了した (枠に人がいる。メッセージのレベル 1 の切断のカード)",
				   vesktop_stopped(true, false)));
		out.push_back(make("status-dropped-vesktop-stopped-recording",
				   "配信中で cordmux の録画中に Vesktop が終了した (録画の 1 文が追加される)", vesktop_stopped(true, true)));
		out.push_back(make("status-dropped-vesktop-stopped-idle",
				   "配信していないときに Vesktop が終了した (VesktopNotRunning の平常の 1 行になる)", vesktop_stopped(false, false)));
	}
	{
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.uninstall.phase = UninstallPhase::Running;
		out.push_back(make("status-uninstall-running", "アンインストールの最中", obs));
	}
	{
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Done;
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.record_dir = "D:\\Videos\\cordmux";
		out.push_back(make("status-uninstall-done", "アンインストールが終わった (フォルダーを削除する案内)", obs));
	}
	{
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Done;
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.record_dir = "D:\\Videos\\cordmux";
		obs.uninstall.leftovers = {{UninstallLeftoverKind::CollectionNotOpened, "配信用 (旧)"}};
		out.push_back(make("status-uninstall-done-unchecked", "アンインストールは終わったが、確認できないシーンコレクションがある", obs));
	}
	{
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		// 途中で止まったときも、製品は削除するフォルダーを入れる (停止したダイアログの「元に戻せなかった
		// 設定」のグループの注意の直後に、場所の行を表示するため)。
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.leftovers = {{UninstallLeftoverKind::Sources, "配信用"},
					   {UninstallLeftoverKind::VencordDir, ""}};
		out.push_back(make("status-uninstall-stopped", "アンインストールが途中で止まった", obs));
	}
	{
		// 停止したカードの場所の行。場所が 1 つの残り。
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		// 途中で止まったときも、製品は削除するフォルダーを入れる (status-uninstall-stopped のコメント)。
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.leftovers = {{UninstallLeftoverKind::LocalDataFolder, "C:\\Users\\user\\AppData\\Local\\cordmux"}};
		out.push_back(make("status-uninstall-stopped-one-place", "アンインストールが途中で止まった (場所が 1 つの残り)", obs));
	}
	{
		// ダイアログの dialog-uninstall-stopped と同じ 4 つの残り (設定のファイルとトークンと
		// ポート番号のフォルダー (場所が 2 つの項目)、ローカルのデータのフォルダー、ソース、
		// Vesktop の設定のファイル)。ダイアログとカードで、項目と場所の行が同じかを
		// 並べて見る。Vesktop の設定の残り (state.json) は、どの場面でもファイル 1 つの場所として
		// is_file を true にする (UninstallLeftoverFact::is_file。製品のアンインストールは state.json を
		// kFile で返す)。現在の表示の規則は VencordDir の行を種類でファイルの行にする
		// (ledger/rules_dock.cpp の uninstall_result) ので画像は変わらないが、場面を製品と同じにしておけば、
		// 表示の規則が is_file を読むように変わっても、画像が製品と食い違わない。
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		// 途中で止まったときも、製品は削除するフォルダーを入れる (status-uninstall-stopped のコメント)。
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles,
					    "C:\\Users\\user\\AppData\\Roaming\\obs-studio\\plugin_config\\cordmux"},
					   {UninstallLeftoverKind::LocalDataFolder, "C:\\Users\\user\\AppData\\Local\\cordmux"},
					   {UninstallLeftoverKind::Sources, "配信用"},
					   {UninstallLeftoverKind::VencordDir, "C:\\Users\\user\\AppData\\Roaming\\Vesktop\\state.json", true}};
		out.push_back(make("status-uninstall-stopped-all", "アンインストールが途中で止まった (ダイアログの場面と同じ 4 つの残り)", obs));
	}
	{
		// 手順書と違う置き方 で止まった。削除するフォルダーが 1 つに決まらない (folder_to_delete が空) ので、
		// 元に戻せなかった設定のグループの注意は、場所の行の前置きではなく dont_delete_files の
		// 段落になる (dock/install_dialog_plan.cpp の uninstall_result_is_loose)。グループの間の間隔を
		// 数えられるように、削除できなかったもの (ソース) と、元に戻せなかった設定 (Vesktop の設定) の
		// 2 つのグループを置く。
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		obs.uninstall.leftovers = {{UninstallLeftoverKind::Sources, "配信用"},
					   {UninstallLeftoverKind::VencordDir, "C:\\Users\\user\\AppData\\Roaming\\Vesktop\\state.json", true}};
		out.push_back(make("status-uninstall-stopped-loose", "アンインストールが途中で止まった (手順書と違う置き方)", obs));
	}
	{
		// 3 つのグループ (削除できなかったもの・元に戻せなかった設定・確認できなかったもの) がそろって
		// 止まった。2 つ目のグループの境の前が、場所の行の [フォルダーを開く] になる形を、カードと
		// ダイアログの両方で描画する。
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles,
					    "C:\\Users\\user\\AppData\\Roaming\\obs-studio\\plugin_config\\cordmux"},
					   {UninstallLeftoverKind::Sources, "配信用"},
					   {UninstallLeftoverKind::VencordDir, "C:\\Users\\user\\AppData\\Roaming\\Vesktop\\state.json", true},
					   {UninstallLeftoverKind::CollectionNotOpened, "配信用 (旧)"}};
		out.push_back(make("status-uninstall-stopped-groups", "アンインストールが途中で止まった (3 つのグループ)", obs));
	}
	// 製品で起きる、アンインストールが途中で止まった形。アンインストールは、残りを返した最初の段階で止まり、その
	// 段階の残りだけを持つ (install/uninstall.cpp の run_uninstall)。段階 (1) は設定のファイルとトークンの
	// フォルダー、段階 (2) はソース、段階 (3) は Vesktop の設定。開けなかった
	// シーンコレクションは、段階をまたいでためる。上の status-uninstall-stopped・-all・-loose・-groups と、
	// ダイアログの dialog-uninstall-stopped・-loose・-groups は、段階 (1)・(2)・(3) の残りを混ぜた合成の場面で、
	// 製品では同じアンインストールで表示されない (グループと場所の行の並べ方を 1 枚で見るための場面である)。
	{
		// 段階 (1) で止まった。設定のファイル (plugin_config の cordmux の下のファイル。バックアップ
		// vencord_dir_backup.json を残して削除する途中で止まった形) と、トークンとポート番号のフォルダーが
		// 残る。段階 (3) はまだ実行していない。「削除できなかったもの」の次の操作は
		// 「もう一度アンインストールしてください。」だけになる (手で削除するとは伝えない)。設定のファイルの
		// 残りはファイル 1 つの場所 (is_file) なので、その場所の行は [ファイルの場所を開く]。トークンの
		// フォルダーはフォルダーのままなので [フォルダーを開く]。
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles,
					    "C:\\Users\\user\\AppData\\Roaming\\obs-studio\\plugin_config\\cordmux\\settings.json", true},
					   {UninstallLeftoverKind::LocalDataFolder, "C:\\Users\\user\\AppData\\Local\\cordmux", false}};
		out.push_back(make("status-uninstall-stopped-settings", "アンインストールが段階 (1) で止まった (設定のファイルとトークンのフォルダー。製品で起きる形)", obs));
	}
	{
		// 段階 (3) で止まった (元に戻せなかった設定だけが残る、製品で起きる形)。段階 (3) がバックアップを
		// 見たときは、バックアップを残した設定のフォルダー (plugin_config の
		// cordmux。UninstallFacts::settings_dir) も示す。注意は「この設定を元に戻すまで、次のフォルダーを
		// 削除しないでください。もう一度アンインストールするときに必要です。」で、場所の行は
		// プラグインのフォルダー、plugin_config の cordmux の順の 2 つ (どちらも [フォルダーを開く])。
		// 削除できなかったもののグループはない。Vesktop の設定の残りはファイル 1 つ
		// (state.json) なので [ファイルの場所を開く]。
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.settings_dir = kSettingsDirKept;
		obs.uninstall.leftovers = {{UninstallLeftoverKind::VencordDir, "C:\\Users\\user\\AppData\\Roaming\\Vesktop\\state.json", true}};
		out.push_back(make("status-uninstall-stopped-not-restored", "アンインストールが段階 (3) で止まった (元に戻せなかった設定だけ。製品で起きる形)", obs));
	}
	// ---- notices の表のすべての行 ----
	{
		Observation obs = connected_in_call();
		obs.contacts = {{"roster", ContactState::Missing}, {"profile", ContactState::Lost}};
		out.push_back(make("notice-contacts", "Discord の内部への依存箇所が 2 つ読めない", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.choices.composite_present = false;
		out.push_back(make("notice-composite-absent", "OBS に cordmux のソースがまだない ([+] から追加する案内)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.choices.receive_scope = ReceiveScope::Everyone; // 枠に載っていない人も録画の対象にする (既定の「枠に載せた人だけ」では対象にならない)
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.enabled_since_us = kNow - 120 * kSec;
		obs.recording.sender_recording = true;
		obs.recording.remaining_seconds = 5 * 3600;
		obs.people[3].sharing = true;
		obs.people[3].last_record_frame_at_us = kNow - kSec / 2;
		out.push_back(make("notice-recording-only", "枠に載せていない人を録画のためだけに受け取っている", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.install.dir = VencordDirState::HashMismatch;
		obs.install.mismatched_files = {"vencordDesktopRenderer.js"};
		out.push_back(make("notice-overwritten-while-connected", "接続している間に、送信側の cordmux が上書きされた", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.versions.discord_side = "0.0.9";
		out.push_back(make("notice-version-restart", "送信側の cordmux が同梱のものより古い (Vesktop の再起動が必要)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.versions.plugin = "0.1.1";
		out.push_back(make("notice-version-mixed", "配布用の zip の中身が混ざっている (cordmux.dll と同梱のもののバージョンが違う)", obs));
	}
	{
		// 検知は、ソースを名前で示せたときだけ Found にする約束
		// (ledger/observation.h の discord_audio_source_name。名前がないと、ドックはメッセージを
		// 表示しない。ledger/rules_dock.cpp の DoubleAudio)。名前は、利用者の OBS のソースの架空の名前。
		// cordmux のソースは配信の出力に出ていて、ミュートでも音量 0% でもない (connected_in_call の値)。
		Observation obs = connected_in_call();
		obs.choices.composite_active = true;
		obs.choices.composite_audible = true;
		obs.choices.discord_audio_source = DiscordAudioSource::Found;
		obs.choices.discord_audio_source_name = "アプリケーション音声キャプチャ (ベータ版)";
		out.push_back(make("notice-double-audio", "OBS が Discord の音声をほかのソースでも取り込んでいるおそれがある", obs));
	}
	{
		Fixture f = make("notice-broadcaster-not-in-call", "メインアカウントが通話に参加していない",
				 connected_in_call());
		f.settings.broadcaster_user_id = fake_id(77);
		out.push_back(f);
	}
	{
		// 枠に載っていて画面共有している 3 人の全員の映像が止まった。製品は、ほかに映像が届いている人がいる
		// 間は、Vesktop で確認するよう勧める文を表示しないので、全員を止める (1 人でも
		// 届いていると、場面の名前と表示が食い違う)。
		Observation obs = connected_in_call();
		obs.people[0].last_video_frame_at_us = kNow - 12 * kSec;
		obs.people[1].last_video_frame_at_us = kNow - 10 * kSec;
		obs.people[2].last_video_frame_at_us = kNow - 9 * kSec;
		out.push_back(make("notice-all-video-stopped", "画面共有している 3 人の全員の映像が止まった (Vesktop で映像を確認する案内)", obs));
	}
	{
		// 枠に載っていない 2 人の、録画の入力の映像が止まった (枠についての文は表示せず、録画についての文を
		// 表示する)。枠に載っている人の映像が届いているとメッセージにならないので、枠に誰も載せない。
		Observation obs = connected_in_call();
		obs.choices.receive_scope = ReceiveScope::Everyone; // 枠に載っていない人も録画の対象にする (既定の「枠に載せた人だけ」では対象にならない)
		for (int i = 0; i < 3; ++i) {
			obs.people[i].slot.reset();
			obs.people[i].sharing = false;
			obs.people[i].last_video_frame_at_us.reset();
		}
		obs.people[2].sharing = true;
		obs.people[2].sharing_since_us = kNow - 600 * kSec;
		obs.people[2].last_record_frame_at_us = kNow - 20 * kSec;
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.phase_since_us = kNow - 1800 * kSec;
		obs.recording.enabled_since_us = kNow - 1800 * kSec;
		obs.recording.sender_recording = true;
		obs.recording.remaining_seconds = 5 * 3600;
		obs.people[3].sharing = true;
		obs.people[3].sharing_since_us = kNow - 600 * kSec;
		obs.people[3].last_record_frame_at_us = kNow - 20 * kSec;
		out.push_back(make("notice-no-video-recording-only", "録画の対象だけの 2 人 (枠に載っていない) の映像が止まった", obs));
	}
	{
		// 両方 (枠に載っている人と、録画の対象だけの人。枠についての文を先に、録画についての文を後に表示する)。
		Observation obs = connected_in_call();
		obs.choices.receive_scope = ReceiveScope::Everyone; // 枠に載っていない人も録画の対象にする (既定の「枠に載せた人だけ」では対象にならない)
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.phase_since_us = kNow - 1800 * kSec;
		obs.recording.enabled_since_us = kNow - 1800 * kSec;
		obs.recording.sender_recording = true;
		obs.recording.remaining_seconds = 5 * 3600;
		obs.people[0].last_video_frame_at_us = kNow - 12 * kSec;
		obs.people[1].last_video_frame_at_us = kNow - 12 * kSec;
		obs.people[2].last_video_frame_at_us = kNow - 12 * kSec;
		obs.people[3].sharing = true;
		obs.people[3].sharing_since_us = kNow - 600 * kSec;
		obs.people[3].last_record_frame_at_us = kNow - 20 * kSec;
		out.push_back(make("notice-no-video-both", "枠に載っている人と録画の対象だけの人の両方の映像が止まった", obs));
	}
	{
		// 枠に載っていて録画もしている全員の映像が止まった。録画の対象の人がいれば、枠に載っているかに
		// 関わらず、録画についての文を追加するので、枠についての文と録画についての文の両方が表示される。
		Observation obs = connected_in_call();
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.phase_since_us = kNow - 1800 * kSec;
		obs.recording.enabled_since_us = kNow - 1800 * kSec;
		obs.recording.sender_recording = true;
		obs.recording.remaining_seconds = 5 * 3600;
		obs.people[0].last_video_frame_at_us = kNow - 12 * kSec;
		obs.people[1].last_video_frame_at_us = kNow - 12 * kSec;
		obs.people[2].last_video_frame_at_us = kNow - 12 * kSec;
		out.push_back(make("notice-no-video-slot-recording", "枠に載っていて録画もしている全員の映像が止まった", obs));
	}
	{
		// エンコードの段階が unavailable の人 (送信側がエンコードできず、映像を送っていない) だけの映像が、
		// 止まったと判断するしきい値を超えた。次の操作は画面共有の始め直しの 1 文だけで、Vesktop で確認する
		// 文は表示されない。行は「映像を表示できません」。people-stages の ren のフレームを古くしないのは、
		// people-stages は行の状態のテキストと 1 行目の人数を数える場面で、メッセージが上端に表示されると、行の
		// 位置と数える前提が変わるためである。
		Observation obs = connected_in_call();
		for (int i = 0; i < 3; ++i) {
			obs.people[i].stage = std::string(kStageUnavailable);
			obs.people[i].last_video_frame_at_us = kNow - 12 * kSec;
		}
		out.push_back(make("notice-no-video-cant-show", "エンコードの段階が unavailable の人だけの映像が止まった (画面共有の始め直しの案内)", obs));
	}
	{
		// エンコードの段階が unavailable の人と、段階が送られてきていない、映像が届かない人が混ざった。
		// 次の操作は、Vesktop で確認する文が先、始め直しの文が後で、どちらも 1 回。
		Observation obs = connected_in_call();
		obs.people[0].last_video_frame_at_us = kNow - 12 * kSec;
		obs.people[1].stage = std::string(kStageUnavailable);
		obs.people[1].last_video_frame_at_us = kNow - 12 * kSec;
		obs.people[2].last_video_frame_at_us = kNow - 12 * kSec;
		out.push_back(make("notice-no-video-mixed-stages",
				   "映像が届かない人と、エンコードの段階が unavailable の人の両方 (Vesktop での確認と始め直しの案内)", obs));
	}
	{
		// 枠に載っていて、画面共有していると分かってから、最初の映像を待つしきい値 (既定値は 5 秒) を超えても
		// 1 枚も届かない人だけ (全員)。その人の枠は空なので、伝えるのは待っている文だけで、止まった画像についての
		// 文は表示しない。解像度は、送信側の configure の値を持ったままにする (configure はフレームより先に
		// 届く。dock/build_observation.cpp)。notice-all-video-stopped のフレームを消さないのは、映像が
		// 止まった人の場面がなくなるためである。
		Observation obs = connected_in_call();
		for (int i = 0; i < 3; ++i)
			obs.people[i].last_video_frame_at_us.reset();
		out.push_back(make("notice-no-video-waiting", "枠に載っている全員の最初の映像が届かない (枠は空)", obs));
	}
	{
		// 映像が止まった人と、最初の映像が届かない人の両方が枠に載っている。止まった画像についての文が先、
		// 待っている文が後で、どちらも 1 回 (次の操作の文と同じ形)。
		Observation obs = connected_in_call();
		obs.people[0].last_video_frame_at_us = kNow - 12 * kSec;
		obs.people[1].last_video_frame_at_us = kNow - 12 * kSec;
		obs.people[2].last_video_frame_at_us.reset();
		out.push_back(make("notice-no-video-mixed-waiting",
				   "映像が止まった人と最初の映像が届かない人の両方 (止まった画像の文と空の枠の文)", obs));
	}
	{
		// 最初にエンコーダを確保するときに、エンコードの段階を最後まで下げても失敗した人 (段階が
		// unavailable で、フレームが 1 枚も届かない)。伝えるのは待っている文で、次の操作は始め直しの文だけ。
		// 行は「映像を表示できません」。名前を notice-no-video-cant-show で始めるので、render_sets.py の、
		// 始め直しの文とそれを伝える行のチェックにも入る。
		Observation obs = connected_in_call();
		for (int i = 0; i < 3; ++i) {
			obs.people[i].stage = std::string(kStageUnavailable);
			obs.people[i].last_video_frame_at_us.reset();
		}
		out.push_back(make("notice-no-video-cant-show-waiting",
				   "エンコードの段階が unavailable の人の最初の映像が届かない (空の枠の文と始め直しの案内)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.enabled_since_us = kNow - 3600 * kSec;
		obs.recording.sender_recording = true;
		obs.recording.remaining_seconds = 14 * 60 + 30;
		out.push_back(make("notice-record-space-low", "録画中に保存先の空きが少なくなった", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.recording.finalize_need_bytes = 48'300'000'000ULL;
		obs.recording.free_space_bytes = 35'100'000'000ULL;
		out.push_back(make("notice-record-finalize-short", "録画ファイルの作成に空きが足りない見込み", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.recording.finalize = FinalizeState::Running;
		obs.recording.finalize_files_done = 3;
		obs.recording.finalize_files_total = 8;
		out.push_back(make("notice-record-finalize-running", "録画を作成している最中 (3 / 8)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.recording.pending_present = true;
		obs.recording.pending_session_count = 1;
		out.push_back(make("notice-record-finalize-pending", "録画ファイルの作成途中の録画が残っている ([作成を再開])",
				   obs));
	}
	{
		// 録画ファイルの作成に失敗した (1 つでも作成できなかった。セグメントファイルは残る)。メッセージのレベル 1 で、
		// 録画のセッションのフォルダーの場所の行と [作成を再開] が付く。録画の側は、失敗した
		// 録画のセッションも作成待ちに数えるので、pending も true にする。フォルダーの名前は、OBS の録画の
		// 既定のファイル名の形。場所は普通の長さ (kUsualRecordDir の下)。
		Observation obs = connected_in_call();
		obs.recording.finalize = FinalizeState::Failed;
		obs.recording.pending_present = true;
		obs.recording.pending_session_count = 1;
		obs.recording.phase = RecordPhase::Idle;
		obs.recording.phase_since_us = kNow - 6 * 60 * kSec;
		obs.recording.session_dir = std::string(kUsualRecordDir) + "\\2000-01-01 12-00-00";
		out.push_back(make("notice-record-finalize-failed", "録画ファイルの作成に失敗した (メッセージのレベル 1。録画のセッションのフォルダーの場所と [作成を再開])",
				   obs));
	}
	{
		// 録画ファイルの作成に失敗した理由が、同梱の FFmpeg を起動できなかったこと。次の操作が、空き容量の確認ではなく、
		// zip の展開し直しになる。名前を notice-record-finalize-failed- で始めないのは、render_sets.py が名前の
		// 先頭で選ぶ場面のチェックの期待 (空き容量の確認の文) を受け継がないためである。
		Observation obs = connected_in_call();
		obs.recording.finalize = FinalizeState::Failed;
		obs.recording.finalize_ffmpeg_unavailable = true;
		obs.recording.pending_present = true;
		obs.recording.pending_session_count = 1;
		obs.recording.phase = RecordPhase::Idle;
		obs.recording.phase_since_us = kNow - 6 * 60 * kSec;
		obs.recording.session_dir = std::string(kUsualRecordDir) + "\\2000-01-01 12-00-00";
		out.push_back(make("notice-record-ffmpeg-unavailable", "録画ファイルの作成に失敗した理由が、同梱の FFmpeg を起動できなかったこと (メッセージのレベル 1。次の操作が zip の展開し直し)",
				   obs));
	}
	{
		// 録画ファイルの作成に失敗した録画のセッションが録画ログに残ったまま、次の録画のセッションを録画している (失敗の
		// メッセージは、録画ファイルの作成に成功するまで残す)。この起動で最後に作成した録画のセッションはない (finalize は
		// NotStarted)。録画の状態のコピーの failed_session_dir が失敗したほうで、session_dir は今録画している
		// 別のほう。メッセージのレベル 1 の場所の行は、失敗したほうのフォルダーにする (今録画しているほうは
		// 表示しない)。名前が notice-record-finalize-failed- で始まるので、render_sets.py が同じ期待
		// (メッセージのレベル 1・見出し・次の操作・失敗したほうの場所の行・押せる [作成を再開]) でチェックする。
		Observation obs = connected_in_call();
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.finalize = FinalizeState::NotStarted;
		obs.recording.pending_present = true;
		obs.recording.pending_session_count = 1;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.phase_since_us = kNow - 10 * 60 * kSec;
		obs.recording.enabled_since_us = kNow - 10 * 60 * kSec;
		obs.recording.sender_recording = true;
		obs.recording.remaining_seconds = 5 * 3600;
		obs.recording.session_dir = std::string(kUsualRecordDir) + "\\2000-01-02 20-00-00";
		obs.recording.failed_session_dir = std::string(kUsualRecordDir) + "\\2000-01-01 12-00-00";
		out.push_back(make("notice-record-finalize-failed-while-recording",
				   "録画ファイルの作成に失敗した録画のセッションが残ったまま、次を録画している (メッセージのレベル 1 のまま。場所の行は失敗したほう)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.enabled_since_us = kNow - 60 * kSec;
		obs.recording.sender_recording = false;
		out.push_back(make("notice-record-not-reached", "録画の状態が送信側に届いていない", obs));
	}
	{
		// 録画を始めようとして、書き込めなかった (段階 StartFailed はメッセージのレベル 1。書き込めなかった場所の
		// 行が表示される)。録画の側は、書き込めなかった場所を unwritable_dir に残す
		// (ledger/rules_dock.cpp の RecordDirUnwritable)。
		Observation obs = connected_in_call();
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.save_dir_writable = false;
		obs.recording.phase = RecordPhase::StartFailed;
		obs.recording.phase_since_us = kNow - 3 * kSec;
		obs.recording.unwritable_dir = "D:\\Videos\\cordmux";
		out.push_back(make("notice-record-dir-unwritable", "録画の保存先に書けない (始めようとして書けなかった。場所の行と [保存先を変更])", obs));
	}
	{
		// 同じ場面を、普通の長さの場所 (OBS の既定の録画の保存先である、利用者の「ビデオ」の下) にしたもの。
		// カードの場所の行の末尾 (書き込み先のフォルダーの名前) が、狭いドックで見えるかを見る。
		Observation obs = connected_in_call();
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.save_dir_writable = false;
		obs.recording.phase = RecordPhase::StartFailed;
		obs.recording.phase_since_us = kNow - 3 * kSec;
		obs.recording.unwritable_dir = kUsualRecordDir;
		out.push_back(make("notice-record-dir-unwritable-usual", "録画の保存先に書けない (普通の長さの場所)", obs));
	}
	{
		// 前の録画のセッションで書き込めなかった場所が残っているが、今は始めようとしていない
		// (メッセージのレベル 2。ledger/rules_dock.cpp の同じ行)。
		Observation obs = connected_in_call();
		obs.recording.phase = RecordPhase::Idle;
		obs.recording.phase_since_us = kNow - 600 * kSec;
		obs.recording.unwritable_dir = "D:\\Videos\\cordmux";
		out.push_back(make("notice-record-dir-unwritable-idle", "前に書けなかった保存先が残っている (今は録画していない。メッセージのレベル 2)", obs));
	}
	{
		// 録画ファイルの作成の進み具合 (今作成しているファイルの千分率がある。「4 番目: 42%」の形)。
		Observation obs = connected_in_call();
		obs.recording.finalize = FinalizeState::Running;
		obs.recording.finalize_files_done = 3;
		obs.recording.finalize_files_total = 8;
		obs.recording.finalize_current_index = 4;
		obs.recording.finalize_permille = 420;
		obs.recording.phase = RecordPhase::Idle;
		obs.recording.phase_since_us = kNow - 5 * 60 * kSec;
		obs.recording.finalize_last_progress_us = kNow - kSec;
		out.push_back(make("notice-record-finalize-percent", "録画を作成している最中 (4 番目が 42%)", obs));
	}
	{
		// 録画ファイルの作成の進み具合が進んでいない (録画の側が、止まったと判断した)。
		Observation obs = connected_in_call();
		obs.recording.finalize = FinalizeState::Running;
		obs.recording.finalize_files_done = 3;
		obs.recording.finalize_files_total = 8;
		obs.recording.finalize_current_index = 4;
		obs.recording.finalize_permille = 420;
		obs.recording.phase = RecordPhase::Idle;
		obs.recording.phase_since_us = kNow - 12 * 60 * kSec;
		obs.recording.finalize_last_progress_us = kNow - 4 * 60 * kSec;
		obs.recording.finalize_stalled = true;
		out.push_back(make("notice-record-finalize-stalled", "録画ファイルの作成が進んでいない (4 番目が 42% のまま)", obs));
	}
	{
		// メッセージのレベル 1 が 2 件 (接続が切れた + 空き容量が尽きて録画が止まった)。1 行目に残りの件数。
		Observation obs = not_connected();
		obs.link.state = LinkState::Disconnected;
		obs.link.connected_at_us = kNow - 900 * kSec;
		obs.link.disconnected_at_us = kNow - 75 * kSec;
		obs.link.reason = DisconnectReason::HeartbeatTimeout;
		obs.recording.stopped_by_space_exhausted = true;
		out.push_back(make("notice-l1-two", "メッセージのレベル 1 が 2 件 (接続が切れた + 空き容量が尽きて録画が止まった)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.recording.stopped_by_space_exhausted = true;
		out.push_back(make("notice-record-stopped-space", "空き容量が尽きて録画が止まった", obs));
	}
	{
		// 空き容量が尽きて録画が止まり、その録画のセッションの録画ファイルの作成も空き容量がなくて失敗した。停止した
		// カードは後で録画ファイルを作成する方法を伝えず、同じドックに表示される録画ファイルの作成の失敗のカードの次の操作と
		// [作成を再開] がそれを伝える、という形を 2 枚が並ぶ画像で確認する。録画の側は止まった
		// 録画のセッションを作成待ちにし、失敗すると録画ログが Failed になる (作成待ちに数える)。この起動で
		// 最後に作成したのがその録画のセッションなので、finalize は Failed で、session_dir も
		// failed_session_dir もそのフォルダー。名前を notice-record-stopped-space と
		// notice-record-finalize-failed- で始めないのは、render_sets.py が名前の先頭で選ぶ場面のチェックの
		// 期待を受け継がないためである (2 枚が並ぶことは、render_sets.py の別のチェックが確認する)。
		Observation obs = connected_in_call();
		obs.recording.stopped_by_space_exhausted = true;
		obs.recording.finalize = FinalizeState::Failed;
		obs.recording.pending_present = true;
		obs.recording.pending_session_count = 1;
		obs.recording.phase = RecordPhase::Idle;
		obs.recording.phase_since_us = kNow - 4 * 60 * kSec;
		obs.recording.session_dir = std::string(kUsualRecordDir) + "\\2000-01-01 12-00-00";
		obs.recording.failed_session_dir = obs.recording.session_dir;
		out.push_back(make("notice-l1-space-finalize-failed",
				   "空き容量が尽きて録画が止まり、その録画ファイルの作成も失敗した (メッセージのレベル 1 の停止と、レベル 2 の作成の失敗)", obs));
	}
	// 1 行目の短い形 (ledger/rules_dock.cpp の kShortHeadlines) のうち、ほかの場面の 1 行目に表示されないものと、
	// 短い形を持つメッセージのレベル 1 が 2 件並ぶ形。名前は notice-short- で始める (ドックの場面は名前の先頭で
	// 選ばれる。dock_fixtures)。render_sets.py の --only は文字列を含む場面を選ぶので、ほかのセットの --only の
	// 文字列を含まない名前にして、ほかのセットの画像を変えない (ドックのすべての場面を描画するセットでは、この
	// 3 つが追加される)。
	{
		// Discord の内部への依存箇所のうち、自分のアカウントを見分ける部分だけが読めない (contact_self の
		// 見出し。notice-contacts は一覧の部分が先に成り立つので、self の見出しが 1 行目に表示されない)。
		Observation obs = connected_in_call();
		obs.contacts = {{"self", ContactState::Missing}};
		out.push_back(make("notice-short-contact-self", "Discord の内部への依存箇所のうち、自分のアカウントの部分だけが読めない", obs));
	}
	{
		// メインアカウントが通話に参加していない (名前を覚えている形。notice-broadcaster-not-in-call は名前の
		// ない形)。名前は、過去の参加者 (seen) から引く (ledger/rules_dock.cpp の BroadcasterNotInCall)。
		// 利用者らしい架空の名前 (kNameMika。通話の 4 人とは別の人)。
		Fixture f = make("notice-short-main-account-named", "メインアカウント (名前を覚えている) が通話に参加していない",
				 [] {
					 Observation obs = connected_in_call();
					 obs.seen.people.push_back(seen(77, kNameMika, 3600 * kSec));
					 return obs;
				 }());
		f.settings.broadcaster_user_id = fake_id(77);
		out.push_back(f);
	}
	{
		// メッセージのレベル 2 が 2 件で、どちらも短い形を持つ (全員の画面共有の映像が届かない + 録画ファイルの作成の失敗)。
		Observation obs = connected_in_call();
		obs.people[0].last_video_frame_at_us = kNow - 20 * kSec;
		obs.people[1].last_video_frame_at_us = kNow - 20 * kSec;
		obs.people[2].last_video_frame_at_us = kNow - 20 * kSec;
		obs.recording.finalize = FinalizeState::Failed;
		obs.recording.pending_present = true;
		obs.recording.pending_session_count = 1;
		obs.recording.phase = RecordPhase::Idle;
		obs.recording.phase_since_us = kNow - 6 * 60 * kSec;
		obs.recording.session_dir = std::string(kUsualRecordDir) + "\\2000-01-01 12-00-00";
		out.push_back(make("notice-short-video-finalize",
				   "メッセージのレベル 2 が 2 件 (映像が届いていない全員 + 録画ファイルの作成の失敗)。どちらも 1 行目の短い形を持つ", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.install.dir = VencordDirState::HashMismatch;
		obs.install.mismatched_files = {"vencordDesktopPreload.js"};
		obs.install.reinstall_failed_files = {"vencordDesktopPreload.js"};
		out.push_back(make("notice-reinstall-failed", "[再インストール] でコピーできなかったファイルがある", obs));
	}
	{
		// 同時に成り立つことがあるメッセージを重ねた場面 (縦にスクロールできるか、並びを読めるか)。
		Observation obs = connected_in_call();
		obs.contacts = {{"sharing", ContactState::Missing}, {"self", ContactState::Lost}};
		obs.install.dir = VencordDirState::HashMismatch;
		obs.install.mismatched_files = {"vencordDesktopRenderer.js"};
		obs.versions.discord_side = "0.0.9";
		obs.choices.discord_audio_source = DiscordAudioSource::Found;
		obs.choices.discord_audio_source_name = "アプリケーション音声キャプチャ (ベータ版)";
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.enabled_since_us = kNow - 3600 * kSec;
		obs.recording.sender_recording = true;
		obs.recording.remaining_seconds = 9 * 60;
		obs.recording.pending_present = true;
		Fixture f = make("notice-many", "メッセージがいくつも同時に表示されている (縦のスクロール)", obs);
		f.settings.broadcaster_user_id = fake_id(77);
		out.push_back(f);
	}
	{
		// メッセージのレベル 1、2、3 がそれぞれ 2 件以上同時にある場面 (1 行目は 1 つだけで、状態タブにすべて置く)。
		// 人は connected_in_call() のまま、録画と音声と Discord の内部の状態だけで重ねる。
		Observation obs = connected_in_call();
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.save_dir_writable = false;
		obs.recording.phase = RecordPhase::StartFailed;
		obs.recording.phase_since_us = kNow - 3 * kSec;
		obs.recording.unwritable_dir = "D:\\Videos\\cordmux";
		obs.recording.stopped_by_space_exhausted = true;
		obs.contacts = {{"sharing", ContactState::Missing}, {"self", ContactState::Lost}};
		obs.choices.discord_audio_source = DiscordAudioSource::Found;
		obs.choices.discord_audio_source_name = "アプリケーション音声キャプチャ (ベータ版)";
		obs.recording.pending_present = true;
		obs.recording.pending_session_count = 1;
		obs.recording.finalize = FinalizeState::Running;
		obs.recording.finalize_files_done = 3;
		obs.recording.finalize_files_total = 8;
		obs.recording.finalize_current_index = 4;
		obs.recording.finalize_permille = 420;
		obs.recording.finalize_last_progress_us = kNow - kSec;
		obs.recording.gaps = {{kNow - 300 * kSec, 4 * kSec}, {kNow - 900 * kSec, 2 * kSec}};
		out.push_back(make("notice-tiers-overlap", "メッセージのレベル 1、2、3 がそれぞれ複数同時にある (1 行目は 1 つ、状態タブにすべて)", obs));
	}
}

// 録画のボタンの 5 つの状態と、1 秒の境。段階 (RecordPhase) と、その段階に入った時刻を、録画の状態のコピーとして
// 入れる (表示の規則の record_button が、テキストと押された見た目を決める)。失敗の状態は、notice-record-dir-unwritable
// (StartFailed) と notice-record-stopped-space (空き容量が尽きた) が描画する。
void add_record_fixtures(std::vector<Fixture> &out)
{
	const auto with_phase = [](RecordPhase phase, std::int64_t since_ago_us, bool on) {
		Observation obs = connected_in_call();
		obs.choices.recording = on ? FeatureSwitch::On : FeatureSwitch::Off;
		obs.recording.phase = phase;
		obs.recording.phase_since_us = kNow - since_ago_us;
		if (phase == RecordPhase::Recording) {
			obs.recording.enabled_since_us = kNow - since_ago_us;
			obs.recording.sender_recording = true;
			obs.recording.remaining_seconds = 5 * 3600;
		}
		return obs;
	};
	out.push_back(make("record-idle", "録画のボタン: 待機 ([録画を開始])", with_phase(RecordPhase::Idle, 600 * kSec, false)));
	out.push_back(make("record-starting-0.5s", "録画のボタン: 開始の処理に入って 0.5 秒 (1 秒までは前のテキスト)",
			   with_phase(RecordPhase::Starting, kSec / 2, true)));
	out.push_back(make("record-starting-1.5s", "録画のボタン: 開始の処理に入って 1.5 秒 (処理中のテキスト)",
			   with_phase(RecordPhase::Starting, 3 * kSec / 2, true)));
	out.push_back(make("record-recording", "録画のボタン: 録画中 (押された見た目)",
			   with_phase(RecordPhase::Recording, 120 * kSec, true)));
	out.push_back(make("record-stopping-0.5s", "録画のボタン: 停止の処理に入って 0.5 秒 (1 秒までは前のテキスト)",
			   with_phase(RecordPhase::Stopping, kSec / 2, false)));
	out.push_back(make("record-stopping-1.5s", "録画のボタン: 停止の処理に入って 1.5 秒 (処理中のテキスト。押された見た目のまま)",
			   with_phase(RecordPhase::Stopping, 3 * kSec / 2, false)));
}

// ドックの残りの場面 (ダミーと、操作の失敗の行。ledger/rules.h の AddedRuleId のすべて)。
void add_dock_extra_fixtures(std::vector<Fixture> &out)
{
	{
		Fixture f = make("status-dummy", "ダミーを表示している (1 行目は普通のテキスト)", connected_in_call());
		f.dummy_on = true;
		out.push_back(f);
	}
	{
		Observation obs = connected_in_call();
		obs.ops.settings_unreadable_at_startup = true;
		out.push_back(make("added-settings-unread", "起動のときに cordmux の設定を読めなかった", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.ops.settings_unsaved = true;
		out.push_back(make("added-settings-unsaved", "cordmux の設定を保存できていない", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.ops.seen_unreadable_at_startup = true;
		out.push_back(make("added-seen-unread", "起動のときに参加者の設定を読めなかった", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.ops.seen_unsaved = true;
		out.push_back(make("added-seen-unsaved", "参加者の設定を保存できていない", obs));
	}
	{
		Observation obs = not_connected();
		obs.install.dir = VencordDirState::NotConfigured;
		obs.ops.vesktop_add_failure = VesktopAddFailure::StateWriteFailed;
		out.push_back(make("added-vesktop-add-failed", "[Vesktopに追加] が失敗した (設定のファイルに書き込めない)", obs));
	}
	{
		// ほかの 7 つの理由 (理由の 1 文のキーは 6 つ。Refused には理由の文がない)。
		const std::pair<VesktopAddFailure, const char *> failures[] = {
			{VesktopAddFailure::StatePathUnknown, "state-missing"},
			{VesktopAddFailure::StateUnreadable, "state-unreadable"},
			{VesktopAddFailure::BundledDirUnknown, "bundled-missing"},
			{VesktopAddFailure::Refused, "refused"},
			{VesktopAddFailure::WritesDisabled, "writes-off"},
			{VesktopAddFailure::NoBackupPlace, "no-backup-place"},
			{VesktopAddFailure::BackupWriteFailed, "backup-write"},
		};
		for (const auto &[failure, name] : failures) {
			Observation obs = not_connected();
			obs.install.dir = VencordDirState::NotConfigured;
			obs.ops.vesktop_add_failure = failure;
			out.push_back(make(std::string("added-vesktop-add-failed-") + name,
					   std::string("[Vesktopに追加] が失敗した (理由: ") + name + ")", obs));
		}
	}
	{
		// [Vesktopから削除] が失敗した (4 つの理由)。押せるのは Vesktop を終了した後なので、Vesktop は起動していない。
		const std::pair<VesktopRemoveFailure, const char *> failures[] = {
			{VesktopRemoveFailure::StateUnreadable, "state-unreadable"},
			{VesktopRemoveFailure::NotRestorable, "not-restorable"},
			{VesktopRemoveFailure::WritesDisabled, "writes-off"},
			{VesktopRemoveFailure::StateWriteFailed, "state-write"},
		};
		for (const auto &[failure, name] : failures) {
			Observation obs = not_connected();
			obs.install.process = VesktopProcess::NotRunning;
			obs.ops.vesktop_remove_failure = failure;
			out.push_back(make(std::string("added-vesktop-remove-failed-") + name,
					   std::string("[Vesktopから削除] が失敗した (理由: ") + name + ")", obs));
		}
	}
	{
		Observation obs = connected_in_call();
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.phase_since_us = kNow - 1800 * kSec;
		obs.recording.enabled_since_us = kNow - 1800 * kSec;
		obs.recording.sender_recording = true;
		obs.recording.remaining_seconds = 5 * 3600;
		obs.recording.gaps = {{kNow - 300 * kSec, 4 * kSec}};
		out.push_back(make("added-record-gap", "録画が途切れた (既に起きた損失)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.seen.discarded_with_marks = 3;
		out.push_back(make("added-seen-discarded", "過去の参加者の設定を、上限を超えたので削除した (既に起きた損失)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.ops.call_action = CallActionPress{CallActionKind::Leave, kNow - 10 * kSec};
		out.push_back(make("added-call-leave-failed", "[退出] を押して時間がたっても、通話から退出していない", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.ops.call_action = CallActionPress{CallActionKind::Move, kNow - 10 * kSec};
		out.push_back(make("added-call-move-failed", "[移動] を押して時間がたっても、メインアカウントの通話に移動していない", obs));
	}
}

void add_people_fixtures(std::vector<Fixture> &out)
{
	{
		Observation obs = connected_in_call();
		obs.call.presence = Presence::NotInCall;
		obs.call.roster_count = 0;
		obs.people.clear();
		out.push_back(make("people-empty", "参加者一覧: 誰もいない", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.call.roster_count = 1;
		obs.people = {showing(1, kNameHanako, 1)};
		out.push_back(make("people-1", "参加者一覧: 1 人 (枠に載っている)", obs));
	}
	out.push_back(make("people-4", "参加者一覧: 4 人 (3 人を枠に載せ、1 人は載せていない)",
			   connected_in_call()));
	{
		// 8 人。枠は 4 つとも埋まっているので、残りの人のチェックボックスは押せず、理由が表示される。
		Observation obs = connected_in_call();
		obs.call.roster_count = 8;
		obs.people = {showing(1, kNameHanako, 1),
			      showing(2, kNameTaro, 2, 1920, 1080, 60.0),
			      showing(3, kNameSakura, 3, 2560, 1440, 29.97),
			      showing(5, kNameLongJa, 4, 1280, 720, 15.5),
			      listening(6, kNameLongEn),
			      listening(7, kNameMika),
			      listening(8, kNameRen),
			      receiver()};
		obs.people[5].sharing = true;
		obs.seen.people.push_back(seen(5, kNameLongJa, 0));
		obs.seen.people.back().always_on = true;
		out.push_back(make("people-8", "参加者一覧: 8 人 (長い名前・絵文字・枠が満杯・サブアカウントの行)", obs));
	}
	{
		// 人の行のすべての状態 (表示している、まだ届かない、止まっている、載せていない)。
		Observation obs = connected_in_call();
		obs.call.roster_count = 6;
		PersonFact waiting = showing(5, kNameRen, 4);
		waiting.last_video_frame_at_us.reset();
		waiting.width.reset();
		waiting.height.reset();
		waiting.fps.reset();
		PersonFact stalled = showing(2, kNameTaro, 2);
		stalled.last_video_frame_at_us = kNow - 20 * kSec;
		obs.people = {showing(6, kNameMika, 1), stalled, waiting, listening(4, kNameKenji),
			      listening(1, kNameHanako)};
		out.push_back(make("people-statuses", "参加者一覧: 行のすべての状態 (表示している・止まっている・まだ届かない・載せていない)", obs));
	}
	{
		// エンコードの段階のすべて (fps を半分、長辺を縮小、ソフトウェアのエンコーダ、映像を送らない、知らない
		// 名前)。
		Observation obs = connected_in_call();
		PersonFact half = showing(1, kNameHanako, 1, 1920, 1080, 30.0);
		half.stage = std::string(kStageHalfFps);
		PersonFact down = showing(2, kNameTaro, 2, 960, 540, 60.0);
		down.stage = std::string(kStageDownscaled);
		PersonFact soft = showing(3, kNameSakura, 3);
		soft.stage = std::string(kStageSoftware);
		// 映像を表示できない人は、前に届いた解像度 (showing の既定値の 1280x720 / 30 FPS) を持ったままにする。
		// 製品では、段階が unavailable の間は送信側が解像度を送らないが、表示の規則 (ledger/rules_dock.cpp の
		// resolution_of) も段階で解像度を捨てる。そのガードを監視するためである (render_sets.py の
		// 「cant-show row shows a resolution」)。解像度を消すと (waiting と同じ形)、表示の規則のガードが
		// 外れても画像に出ず、監視にならない。
		PersonFact gone = showing(5, kNameRen, 4);
		gone.stage = std::string(kStageUnavailable);
		PersonFact other = listening(4, kNameKenji);
		other.stage = std::string("experimental");
		obs.people = {half, down, soft, gone, other};
		obs.call.roster_count = 5;
		out.push_back(make("people-stages", "参加者一覧: エンコードの段階のすべて", obs));
	}
	{
		// 枠に載っていないが画面共有していてエンコードの段階がある人と、最初の映像を待っている人。
		// dock/build_observation.cpp は、枠を見ずに configure のストリームから段階と解像度を付けるので、
		// どちらも解像度を持つ形にする。画質のテキストは、載せていない人の行にも表示する (載せる前に、
		// 載せても表示できないと分かる)。解像度のテキストは、最初のフレームが届いてから表示する (フレームの
		// ないストリームの解像度は、届いていない映像の解像度を表すため)。hanako は比べる相手 (枠に
		// 載っていてフレームが届いた。画質と解像度のテキストの両方が表示される。これがないと、どちらも
		// 表示しない実装でもチェックが通ってしまう)。Taro は枠に載っていて、画面共有を始めて 2 秒
		// (最初のフレームを待つしきい値の内側なので、メッセージは表示されず、行は「映像待ち」)。
		// ren と sakura は枠に載っていない (ren は段階が unavailable、sakura は縮小)。解像度は参加者ごとに別の
		// 値にして、アクセシブルな説明とツールチップから値で探せるようにする。名前を people-stages で
		// 始めないのは、people-stages の 5 人を前提にした render_sets.py のチェックとセットの画像を
		// 変えないためである。
		Observation obs = connected_in_call();
		PersonFact framed = showing(1, kNameHanako, 1, 960, 540, 30.0);
		framed.stage = std::string(kStageDownscaled);
		PersonFact waiting = showing(2, kNameTaro, 2, 1600, 900, 60.0);
		waiting.last_video_frame_at_us.reset();
		waiting.sharing_since_us = kNow - 2 * kSec;
		PersonFact cant_show = listening(5, kNameRen);
		cant_show.sharing = true;
		cant_show.sharing_since_us = kNow - 60 * kSec;
		cant_show.stage = std::string(kStageUnavailable);
		cant_show.width = 1366;
		cant_show.height = 768;
		cant_show.fps = 30.0;
		PersonFact reduced = listening(3, kNameSakura);
		reduced.sharing = true;
		reduced.sharing_since_us = kNow - 60 * kSec;
		reduced.stage = std::string(kStageDownscaled);
		reduced.width = 1152;
		reduced.height = 648;
		reduced.fps = 24.0;
		obs.people = {framed, waiting, cant_show, reduced, listening(4, kNameKenji)};
		obs.call.roster_count = 5;
		out.push_back(make("people-unslotted-stages",
				   "参加者一覧: 枠に載っていない人のエンコードの段階と、最初の映像を待つ人の解像度", obs));
	}
	{
		// 「自動で載せる」が付いた人と、付いていない人。
		Observation obs = connected_in_call();
		obs.seen.people[0].always_on = true;
		obs.seen.people[3].always_on = true;
		out.push_back(make("people-always-on", "参加者一覧: 「自動で載せる」が付いている行と付いていない行", obs));
	}
	{
		// Taro を全画面で表示している。1 行目は「全画面: Taro」と [元に戻す]。Taro の行のメニューの [全画面で表示] には
		// チェックが付き、画面共有していない kenji の行の項目は押せない。
		Observation obs = connected_in_call();
		obs.choices.focus_user_id = fake_id(2);
		out.push_back(make("people-full-screen",
				   "参加者一覧: 1 人を全画面で表示している (1 行目に [元に戻す]、行のメニューの項目にチェック)", obs));
	}
	{
		// 使う声を置き換えた行 (名前の下に「声: %1」) と、使う声の人が通話に参加していない行。
		Observation obs = connected_in_call();
		obs.call.roster_count = 5;
		obs.people.push_back(receiver());
		obs.seen.people[1].voice_source_user_id = fake_id(4); // Taro の声は kenji
		SeenPerson gone = seen(9, "yuki", 3 * 86400 * kSec);
		obs.seen.people.push_back(gone);
		obs.seen.people[2].voice_source_user_id = fake_id(9); // さくら の声は、通話に参加していない yuki
		out.push_back(make("people-voice-source", "参加者一覧: 使う声を置き換えた行 (使う声の人が通話に参加していない行を含む)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.call.roster_count = 5;
		obs.people.push_back(receiver());
		obs.seen.people[1].voice_source_user_id = fake_id(4);
		Fixture f = make("people-voice-menu", "参加者一覧: 「使う声」のメニュー (置き換えていない人の行)", obs);
		out.push_back(f);
		Fixture g = make("people-voice-menu-reset", "参加者一覧: 「使う声」のメニュー (置き換えた人の行。押せない項目に理由)", obs);
		out.push_back(g);
	}
	{
		// 合成のソースがあるシーンコレクション: 人の行にミュートのボタンが付き、行のメニューに音量のスライダーが付く。
		// hanako: ミュートなし、画面共有の音あり (ミュート解除)。Taro: メインアカウントで声はミュート (既定)。
		// さくら: 画面共有の音あり (ミュート中)、声の音量 60%、画面共有の音量 150%。kenji: ミュートなし。
		// ren: 声をミュート。
		Observation obs = connected_in_call();
		obs.choices.composite_present = true;
		obs.call.roster_count = 6;
		obs.people[0].audio.has_screen_audio = true;
		obs.people[1].audio.voice_muted = true;
		obs.people[2].audio.has_screen_audio = true;
		obs.people[2].audio.screen_muted = true;
		obs.people[2].audio.voice_volume_pct = 60;
		obs.people[2].audio.screen_volume_pct = 150;
		PersonFact ren = listening(5, kNameRen);
		ren.audio.voice_muted = true;
		obs.people.push_back(ren);
		obs.people.push_back(receiver());
		obs.seen.people.push_back(seen(5, kNameRen, 0));
		Fixture f = make("people-audio", "参加者一覧: 合成のソースがある (ミュートのボタンと音量のスライダーを置く)", obs);
		f.settings.broadcaster_user_id = fake_id(2);
		out.push_back(f);

		// 参加者タブの見た目の状態を 1 枚で見る場面。接続が切れた場面は、下の people-off にある。
		Observation states = obs;
		for (PersonFact &person : states.people) {
			if (person.user_id == fake_id(4))
				person.voice_arriving = false; // まだ声が届いていない
		}
		states.seen.people.push_back(seen(4, kNameKenji, 0));
		for (ledger::SeenPerson &person : states.seen.people) {
			if (person.user_id == fake_id(1))
				person.always_on = true; // [自動] のタグ
		}
		Fixture g = make("people-tab-states", "参加者タブ: タグ、位置、話している人、まだ声が届いていない人、ミュート", states);
		g.settings.broadcaster_user_id = fake_id(2);
		g.speaking = {fake_id(1)};
		out.push_back(g);

		// メインアカウントとサブアカウントの行の通話の名前。[移動] と [退出] の押せる条件も見る。
		const auto accounts_scene = [&out](const char *id, const char *note, const Observation &scene) {
			Fixture account = make(id, note, scene);
			account.settings.broadcaster_user_id = fake_id(2);
			out.push_back(account);
		};
		Observation same_call = obs;
		same_call.call.name = "雑談 / 配信サーバー";
		accounts_scene("people-accounts-same-call", "アカウントの行: メインアカウントとサブアカウントが同じ通話", same_call);

		Observation long_call = obs;
		long_call.call.name = "とても長い名前のボイスチャンネル / とても長い名前のコミュニティサーバー";
		accounts_scene("people-accounts-long-call", "アカウントの行: 通話の名前が長い (末尾を省略する)", long_call);

		Observation other_call = obs;
		other_call.people.erase(std::remove_if(other_call.people.begin(), other_call.people.end(),
						       [](const PersonFact &person) { return person.user_id == fake_id(2); }),
					other_call.people.end());
		other_call.call.name = "雑談 / 配信サーバー";
		other_call.call.main_presence = Presence::InCall;
		other_call.call.main_name = "ロビー / 配信サーバー";
		other_call.call.main_call_joinable = true;
		accounts_scene("people-accounts-other-call", "アカウントの行: メインアカウントが別の通話 ([移動] が押せる)", other_call);

		Observation other_server = other_call;
		other_server.call.main_name = "ロビー / 別のサーバー";
		other_server.call.main_call_joinable = false;
		accounts_scene("people-accounts-other-server", "アカウントの行: サブアカウントが入れないサーバーの通話 ([移動] が押せない)", other_server);

		Observation sub_out = other_call;
		sub_out.call.presence = Presence::NotInCall;
		sub_out.call.name.clear();
		sub_out.call.roster_count = 0;
		sub_out.people.clear();
		accounts_scene("people-accounts-sub-no-call", "アカウントの行: サブアカウントが通話の外 ([移動] だけ押せる)", sub_out);

		// 接続が切れても行を消さず、薄くする。枠に載っている人は、枠を空にするまでの間 people に残る (画面共有の事実は消える)。
		Observation off = obs;
		off.last_known_people = off.people;
		off.people.clear();
		for (const PersonFact &person : off.last_known_people) {
			if (!person.slot.has_value())
				continue;
			PersonFact slotted;
			slotted.user_id = person.user_id;
			slotted.display_name = person.display_name;
			slotted.slot = person.slot;
			off.people.push_back(slotted);
		}
		off.link.state = ledger::LinkState::Disconnected;
		Fixture h = make("people-off", "参加者タブ: Vesktop との接続が切れた (行は残り、薄くなって押せない)", off);
		h.settings.broadcaster_user_id = fake_id(2);
		out.push_back(h);

		// 人数が多い場面 (30 人)。枠は埋まっていて、載っていない人のチェックは薄い。
		Observation many = thirty_people();
		out.push_back(make("people-30", "参加者タブ: 30 人 (枠が埋まっている)", many));
	}
	{
		// 長い名前と、行の状態のテキストがある行にミュートのボタンを置いたとき (狭いドックで、名前の省略、状態の
		// テキスト、ボタンが重ならないか)。
		Observation obs = connected_in_call();
		obs.choices.composite_present = true;
		PersonFact long_ja = showing(1, kNameLongJa, 1);
		long_ja.audio.has_screen_audio = true;
		long_ja.audio.screen_muted = true;
		PersonFact waiting = showing(2, kNameLongEn, 2, 1600, 900, 60.0);
		waiting.last_video_frame_at_us.reset();
		waiting.sharing_since_us = kNow - 2 * kSec;
		waiting.audio.has_screen_audio = true;
		waiting.audio.voice_muted = true;
		PersonFact reduced = listening(3, kNameSakura);
		reduced.sharing = true;
		reduced.sharing_since_us = kNow - 60 * kSec;
		reduced.stage = std::string(kStageDownscaled);
		reduced.width = 1152;
		reduced.height = 648;
		reduced.fps = 24.0;
		obs.people = {long_ja, waiting, reduced, listening(4, kNameKenji)};
		obs.seen.people = {seen(1, kNameLongJa, 0), seen(2, kNameLongEn, 0), seen(3, kNameSakura, 0), seen(4, kNameKenji, 0)};
		obs.call.roster_count = 4;
		out.push_back(make("people-audio-long", "参加者一覧: 長い名前と状態のテキストがある行に、ミュートのボタンを置く", obs));
	}
	{
		// 合成のソースがない: ミュートのボタンも音量のスライダーも置かない。
		Observation obs = connected_in_call();
		obs.choices.composite_present = false;
		obs.people[0].audio.has_screen_audio = true;
		obs.people.push_back(receiver());
		obs.call.roster_count = 5;
		out.push_back(make("people-audio-none", "参加者一覧: 合成のソースがない (ミュートのボタンを置かない)", obs));
	}
	{
		// アンインストールを始めた後は人の行を作らず、空の一覧の 1 文になる
		// (ledger/rules_dock.cpp の decide_dock。EmptyKind::Uninstalled)。
		Observation obs = connected_in_call();
		obs.choices.feature = FeatureSwitch::Off;
		obs.uninstall.phase = UninstallPhase::Running;
		out.push_back(make("people-after-uninstall", "参加者一覧: アンインストールを始めた後の空の一覧", obs));
	}
	{
		// 通話にほかの人がいない (サブアカウントだけ。空の一覧の 3 つ目の理由 EmptyKind::Alone)。
		Observation obs = connected_in_call();
		obs.call.roster_count = 2;
		obs.people = {receiver()};
		out.push_back(make("people-alone", "参加者一覧: 通話にほかの人がいない (サブアカウントだけ)", obs));
	}
	{
		// 押した直後に空きがなくて載せられなかった行 (行のそばに短い結果を表示する)。枠は 4 つとも使用中。
		Observation obs = connected_in_call();
		obs.call.roster_count = 6;
		obs.people = {showing(1, kNameHanako, 1), showing(2, kNameTaro, 2, 1920, 1080, 60.0),
			      showing(3, kNameSakura, 3), showing(5, kNameRen, 4), listening(4, kNameKenji),
			      listening(7, kNameMika)};
		obs.people[4].sharing = true;
		obs.ops.last_slot_refused_user_id = fake_id(4);
		out.push_back(make("people-refused", "参加者一覧: 押した直後に空きがなくて載せられなかった行がある", obs));
	}
	{
		// 自分の枠 (ミラーのソース) を表示している間は、Discord の人を載せられるのは 3 人まで。3 人を
		// 載せているので、載せていない人のチェックボックスは押せず、理由が一覧の上に表示される。
		Observation obs = connected_in_call();
		obs.choices.self_frame_shown = true;
		out.push_back(make("people-self-frame", "参加者一覧: 自分の枠を表示している間は、4 人目を載せられない (3 人まで)", obs));
	}
	{
		// 自分の枠に映すソースが結び付いていて、自分の枠を出している。メインアカウント (Taro) の行のチェックが付く。
		// メインアカウントの Discord の画面は枠に載せない (行に枠の番号を持たない)。
		Observation obs = connected_in_call();
		obs.people[1].slot.reset();
		obs.people[1].is_self_frame = true;
		obs.choices.self_frame_selected = true;
		obs.choices.self_frame_bound = true;
		obs.choices.self_frame_on = true;
		obs.choices.self_frame_shown = true;
		obs.choices.self_frame_share = SelfShareState::On;
		Fixture f = make("people-self-frame-on", "参加者一覧: 自分の枠を出している (メインアカウントの行のチェックが付く)", obs);
		f.settings.broadcaster_user_id = fake_id(2);
		out.push_back(f);
	}
	{
		// 自分の枠を出していない。メインアカウントの行のチェックが外れていて、押せる。
		Observation obs = connected_in_call();
		obs.people[1].slot.reset();
		obs.people[1].is_self_frame = true;
		obs.choices.self_frame_selected = true;
		obs.choices.self_frame_bound = true;
		obs.choices.self_frame_share = SelfShareState::Off;
		Fixture f = make("people-self-frame-off", "参加者一覧: 自分の枠を出していない (メインアカウントの行のチェックが外れている)", obs);
		f.settings.broadcaster_user_id = fake_id(2);
		out.push_back(f);
	}
	{
		// Discord の人が枠を 4 つとも埋めている。自分の枠を出していないので、メインアカウントの行のチェックは押せず、
		// 理由が一覧の上に表示される。
		Observation obs = connected_in_call();
		obs.call.roster_count = 5;
		obs.people = {showing(1, kNameHanako, 1), showing(2, kNameTaro, 2, 1920, 1080, 60.0),
			      showing(3, kNameSakura, 3), showing(5, kNameRen, 4), showing(4, kNameKenji, 2)};
		obs.people[1].slot.reset();
		obs.people[1].is_self_frame = true;
		obs.choices.self_frame_selected = true;
		obs.choices.self_frame_bound = true;
		obs.choices.self_frame_share = SelfShareState::Off;
		Fixture f = make("people-self-frame-full", "参加者一覧: 枠が 4 つとも埋まっていて、自分の枠のチェックが押せない", obs);
		f.settings.broadcaster_user_id = fake_id(2);
		out.push_back(f);
	}
	{
		// メインアカウントだけの構成。自分の行は Vesktop のアカウントで、チェックだけを出す (メニューと音量は出さない)。
		Observation obs = connected_in_call();
		obs.call.roster_count = 5;
		obs.people.push_back(receiver());
		obs.people.back().is_self_frame = true;
		obs.choices.self_frame_selected = true;
		obs.choices.self_frame_bound = true;
		obs.choices.self_frame_on = true;
		obs.choices.self_frame_shown = true;
		obs.choices.self_frame_share = SelfShareState::On;
		out.push_back(make("people-self-frame-solo", "参加者一覧: メインアカウントだけの構成。自分の行にチェックと、[自動で載せる] だけのメニューを出す", obs));
	}
	{
		// メインアカウントだけの構成で、自分の枠に映す映像を OBS から選んでいない。自分の行のチェックは押せず、理由が出る。
		Observation obs = connected_in_call();
		obs.call.roster_count = 5;
		obs.people.push_back(receiver());
		obs.people.back().is_self_frame = true;
		out.push_back(make("people-self-frame-solo-no-video", "参加者一覧: メインアカウントだけの構成で、映像を選んでいない (自分の行のチェックが押せない)", obs));
	}
	{
		// 選び方が「自動」(行のメニューの「自動で載せる」が押せず、理由がツールチップに表示される)。
		Fixture f = make("people-assign-auto", "参加者一覧: 選び方が「自動」(「自動で載せる」の項目が押せない)",
				 connected_in_call());
		f.settings.assign_mode = config::AssignMode::Auto;
		out.push_back(f);
	}
}

// 名前を見分けられるかを確認する場面。名前の長さ 3・16・32 文字と絵文字、先頭が同じ名前、Discord での名前が同じ 2 人
// (ユーザー名で見分ける)。
void add_name_fixtures(std::vector<Fixture> &out)
{
	{
		Observation obs = connected_in_call();
		obs.call.roster_count = 4;
		obs.people = {showing(1, "ren", 1), showing(2, "Sakurazaka_Haru1", 2),
			      showing(3, "とても長い表示名の人がいるときの例です、三十二文字列まで続きますね。", 3),
			      listening(4, "Mika 🌸✨ 配信中")};
		out.push_back(make("rows-name-lengths", "参加者一覧: 名前 3 文字・16 文字・32 文字と絵文字", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.call.roster_count = 4;
		// 2 人目は、15 em と 20 em の幅で 1 人目と同じテキスト (Sagisawa_Yuika_s...) に省略される名前
		// (真ん中を省略する処理を通すため)。
		obs.people = {showing(1, "Sagisawa_Yuika_stream", 1), showing(2, "Sagisawa_Yuika_sub_2", 2),
			      listening(3, "やまだたろう"), listening(4, "やまだたかし")};
		out.push_back(make("rows-same-prefix", "参加者一覧: 先頭が同じ名前 (真ん中を省略して見分ける)", obs));
	}
	{
		// cordmux での名前を付けた人 (2 行目に Discord での名前)。
		Observation obs = connected_in_call();
		obs.people[1].display_name = std::string("たろう (配信用)");
		obs.people[1].discord_name = std::string(kNameTaro);
		obs.seen.people[1].name_override = std::string("たろう (配信用)");
		out.push_back(make("rows-override", "参加者一覧: cordmux での名前を付けた人 (2 行目に Discord での名前)", obs));
	}
	{
		// 文字の上下の見切れ。「髙」「齋」、結合文字 (濁点の結合文字、e と結合するアクセント記号)、絵文字
		// (単独、肌の色、家族の結合)、ディセンダーのある英字。
		Observation obs = connected_in_call();
		obs.call.roster_count = 6;
		obs.people = {showing(1, "髙橋 齋藤", 1), showing(2, "がくせい e\xCC\x81" "clair", 2),
			      showing(3, "🎮 👍🏽 👨‍👩‍👧", 3), listening(4, "gypsy jumping quiz"),
			      listening(5, "Ｗｉｄｅ ｊｐ"), listening(6, "Ångström Qjpg")};
		out.push_back(make("rows-glyphs", "参加者一覧: 文字の上下の見切れの確認 (漢字・結合文字・絵文字・ディセンダーのある文字)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.call.roster_count = 3;
		PersonFact a = showing(1, "taro", 1);
		a.username = std::string("taro_1");
		PersonFact b = listening(2, "taro");
		b.username = std::string("taro_2");
		obs.people = {a, b, listening(3, kNameKenji)};
		out.push_back(make("rows-same-name", "参加者一覧: Discord での名前が同じ 2 人 (2 行目のユーザー名で見分ける)", obs));
	}
	{
		// 2 行目に 2 つの事実が並ぶ行 (2 行目を縮める順を確認する)。
		//   - 同じ名前の 2 人の片方がほかの人の声を使う: 「声: kenji」と「ユーザー名: ...」。ユーザー名は
		//     末尾だけが違う (見分けるための末尾が残るかを数える)。
		//   - cordmux での名前と使う声を両方持つ人: 「声: yuki (通話に参加していません)」と
		//     「Discordでの名前: ...」(狭いときは、Discord での名前の部分を先に省く)。
		Observation obs = connected_in_call();
		obs.call.roster_count = 4;
		PersonFact a = showing(1, "taro", 1);
		a.username = std::string("taro_yamada_stream_1");
		PersonFact b = listening(2, "taro");
		b.username = std::string("taro_yamada_stream_2");
		PersonFact c = showing(3, "さくら (配信用)", 2);
		c.discord_name = std::string(kNameSakura);
		obs.people = {a, b, c, listening(4, kNameKenji)};
		obs.seen.people[1].voice_source_user_id = fake_id(4); // 2 人目の taro の声は kenji
		obs.seen.people[2].name_override = std::string("さくら (配信用)");
		obs.seen.people[2].voice_source_user_id = fake_id(9); // さくら の声は、通話に参加していない yuki
		obs.seen.people.push_back(seen(9, "yuki", 3 * 86400 * kSec));
		out.push_back(make("rows-line2-two-facts", "参加者一覧: 2 行目に 2 つの事実が並ぶ行 (声とユーザー名、声と Discord での名前)", obs));
	}
	{
		// 同じ形で、先頭の文字の幅が広い人 (最小幅で、2 つの事実の先頭の幅の合計が 2 行目の幅を超える
		// 人)。ユーザー名は、Discord の規則 (英小文字・数字・_ と .) のうち幅の広い文字 (w と m) で始まり
		// 終わる形にし、使う声の人の名前は全角の英字で始める (表示名は文字を選ばない。rows-glyphs の架空の
		// 名前)。製品は、両方の先頭が入らない幅では、ユーザー名をまるごと省き、声を残す
		// (1 行目に名前があるので、ユーザー名は同じ名前の人を見分ける補い)。
		Observation obs = connected_in_call();
		obs.call.roster_count = 4;
		PersonFact a = showing(1, "taro", 1);
		a.username = std::string("wakamatsu_momo_stream_w");
		PersonFact b = listening(2, "taro");
		b.username = std::string("wakamatsu_momo_stream_m");
		const char *const wide = "Ｗｉｄｅ ｊｐ";
		obs.people = {a, b, showing(3, kNameSakura, 2), listening(7, wide)};
		obs.seen.people = {seen(1, "taro", 0), seen(2, "taro", 0), seen(3, kNameSakura, 0), seen(7, wide, 0)};
		obs.seen.people[1].voice_source_user_id = fake_id(7); // 2 人目の taro の声は全角の名前の人
		out.push_back(make("rows-line2-wide-heads", "参加者一覧: 2 行目に声とユーザー名が並び、両方の先頭の文字の幅が広い", obs));
	}
}

void add_seen_fixtures(std::vector<Fixture> &out)
{
	{
		Observation obs = connected_in_call();
		out.push_back(make("seen-empty", "過去の参加者: いない (今通話に参加している人は、ここに表示されない)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.people.clear();
		obs.call.presence = Presence::NotInCall;
		obs.call.roster_count = 0;
		obs.seen.people = {seen(1, kNameHanako, 2 * 86400 * kSec), seen(2, kNameTaro, 3600 * kSec),
				   seen(3, kNameSakura, 40 * 86400 * kSec), seen(5, kNameLongJa, 7 * 86400 * kSec),
				   seen(6, kNameLongEn, 300 * kSec), seen(7, kNameMika, 86400 * kSec)};
		obs.seen.people[0].always_on = true;
		obs.seen.people[1].name_override = std::string("たろう (配信用)");
		obs.seen.people[3].always_on = true;
		obs.seen.people[3].voice_source_user_id = fake_id(1);
		obs.seen.people[5].name_override = std::string("ミカ");
		out.push_back(make("seen-several", "過去の参加者: 6 人 (自動で載せる・名前の変更・使う声・長い名前)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.people.clear();
		obs.seen.people = {seen(1, kNameHanako, 2 * 86400 * kSec), seen(2, kNameTaro, 3600 * kSec)};
		obs.seen.discarded_with_marks = 3;
		out.push_back(make("seen-discarded", "過去の参加者: 上限を超えたので、設定のある行を削除した", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.people.clear();
		obs.choices.feature = FeatureSwitch::Off;
		obs.uninstall.phase = UninstallPhase::Running;
		obs.seen.people = {seen(1, kNameHanako, 2 * 86400 * kSec), seen(2, kNameTaro, 3600 * kSec)};
		obs.seen.people[0].voice_source_user_id = fake_id(2);
		obs.seen.people[1].always_on = true;
		out.push_back(make("seen-after-uninstall", "過去の参加者: アンインストールの後 (操作できず、理由が表示される)", obs));
	}
	{
		// 参加者の設定ウィンドウ: 現在の参加者 4 人と過去の参加者 3 人 (グループの見出しと列の見出しの関係を
		// 見る)。現在の参加者の 1 人は cordmux での名前があり、1 人はほかの人 (長い名前) の声を使う (使う声の
		// 列の幅を見る)。現在の参加者の 1 人と過去の参加者の 1 人は「通話のみ時ラベル非表示」を付けてある (チェックの付いた形)。
		Observation obs = connected_in_call();
		obs.people[3] = listening(5, kNameLongJa);
		obs.people[1].display_name = std::string("たろう (配信用)");
		obs.seen.people = {seen(1, kNameHanako, 0), seen(2, kNameTaro, 0), seen(3, kNameSakura, 0),
				   seen(5, kNameLongJa, 0), seen(6, kNameLongEn, 300 * kSec), seen(7, kNameMika, 86400 * kSec),
				   seen(8, kNameRen, 9 * 86400 * kSec)};
		obs.seen.people[1].name_override = std::string("たろう (配信用)");
		obs.seen.people[2].voice_source_user_id = fake_id(5);
		obs.seen.people[0].always_on = true;
		obs.seen.people[3].roster_hidden = true;
		obs.seen.people[6].roster_hidden = true;
		// 通話に参加した時刻: 参加者ごとに違う (最近入った人が上に並ぶ)。さくらとはなこは同じ時刻 (名前の順)。
		obs.seen.call_joined_at_us[fake_id(1)] = kNow - 5 * 60 * kSec;
		obs.seen.call_joined_at_us[fake_id(2)] = kNow - 40 * 60 * kSec;
		obs.seen.call_joined_at_us[fake_id(3)] = kNow - 5 * 60 * kSec;
		obs.seen.call_joined_at_us[fake_id(5)] = kNow - 3 * 3600 * kSec;
		out.push_back(make("participants-current",
				   "参加者の設定: 現在の参加者 4 人と過去の参加者 3 人 (名前の変更・ほかの人の声・通話のみ時ラベル非表示を付けた人)", obs));
		// 見出しで並べ替えた状態 (列は ParticipantsWindow::Column の値)。
		Fixture by_name = make("participants-sort-name", "参加者の設定: 名前の列で昇順に並べた状態", obs);
		by_name.participants_sort_column = 0;
		by_name.participants_sort_ascending = true;
		out.push_back(by_name);
		Fixture last_seen_old_first =
			make("participants-sort-last-seen-asc", "参加者の設定: 最終参加日時の列で古い順に並べた状態", obs);
		last_seen_old_first.participants_sort_column = 5;
		last_seen_old_first.participants_sort_ascending = true;
		out.push_back(last_seen_old_first);
		Fixture by_rename = make("participants-sort-rename-desc", "参加者の設定: cordmuxでの名前の列で降順に並べた状態", obs);
		by_rename.participants_sort_column = 2;
		by_rename.participants_sort_ascending = false;
		out.push_back(by_rename);
		// どの列でも、昇順と降順で並べた状態 (列の見出しの文字と並べ替えの三角が、灰色の部分の中央に収まるかを見る)。
		for (int column = 0; column < 6; ++column) {
			for (const bool ascending : {true, false}) {
				const std::string id = "participants-sort-c" + std::to_string(column) + (ascending ? "-asc" : "-desc");
				const std::string title = "参加者の設定: 列 " + std::to_string(column) + " で" +
							  (ascending ? "昇順" : "降順") + "に並べた状態 (見出しの三角の位置を見る)";
				Fixture sorted = make(id.c_str(), title.c_str(), obs);
				sorted.participants_sort_column = column;
				sorted.participants_sort_ascending = ascending;
				out.push_back(sorted);
			}
		}
		// 設定の「画面共有していない参加者」の [表示] がオフ: 列「通話のみ時ラベル非表示」が全員で押せない。
		Observation off = obs;
		off.choices.roster_show = false;
		out.push_back(make("participants-roster-off", "参加者の設定: 設定の [表示] がオフ (列「通話のみ時ラベル非表示」が全員で押せない)", off));
		// [Botを表示] がオフ: Bot の行 (3 人目) だけ、列「通話のみ時ラベル非表示」が押せない。
		Observation bots = obs;
		bots.choices.roster_show_bots = false;
		bots.people[2].is_bot = true;
		out.push_back(make("participants-bots-off", "参加者の設定: [Botを表示] がオフ (Bot の行だけ列「通話のみ時ラベル非表示」が押せない)", bots));
	}
}

void add_settings_fixtures(std::vector<Fixture> &out)
{
	{
		Observation obs = connected_in_call();
		out.push_back(make("settings-default", "設定: 既定値 (Vesktop が起動しているので、アンインストールのボタンを押せない)", obs));
	}
	{
		// 保存先の入力欄が空で、空のまま使ったときに書き込む場所が普通の長さ (OBS の既定の録画の
		// 保存先である、利用者の「ビデオ」の下の cordmux)。プレースホルダーに場所の全文が見えるかを見る。
		Observation obs = connected_in_call();
		Fixture f = make("settings-record-dir-usual", "設定: 保存先の入力欄が空 (書き込む場所が普通の長さ)", obs);
		f.record_dir_default = kUsualRecordDir;
		out.push_back(f);
	}
	{
		// 保存先を自分で選んだ場面。入力欄にはその場所 (D:\hoge) が入り、録画のたびに、その中へ開始日時の名前のフォルダー
		// (D:\hoge\<開始日時>) を作る。
		Observation obs = connected_in_call();
		Fixture f = make("settings-record-dir-chosen", "設定: 保存先に D:\\hoge を選んだ", obs);
		f.settings.record_dir = "D:\\hoge";
		f.record_dir_default = kUsualRecordDir;
		out.push_back(f);
	}
	{
		Observation obs = connected_in_call();
		Fixture f = make("settings-changed", "設定: すべての項目を既定値から変えた", obs);
		f.settings.assign_mode = config::AssignMode::Auto;
		f.settings.broadcaster_user_id = fake_id(77);
		f.settings.decoder = config::DecoderChoice::D3D11VA;
		f.settings.screen_audio_offset_ms = -120;
		f.settings.voice_audio_delay_ms = 45;
		f.settings.dock_poll_ms = 250;
		f.settings.record_dir = "D:\\Videos\\cordmux\\配信の録画";
		f.settings.record_output_pcm = config::RecordPcmFormat::Int24;
		f.settings.record_space_warn_minutes = 60;
		f.settings.limit_max_long_edge = 1280;
		f.settings.limit_fps_divisor = 2;
		f.settings.encoder_preference = config::EncoderPreference::Software;
		f.settings.encode_priority = config::EncodePriority::Quality;
		f.settings.receive_scope = config::ReceiveScope::Everyone;
		f.settings.record_with_stream = true;
		f.settings.confirm_while_streaming = false;
		f.settings.layout_hold_idle_ms = 4000;
		f.dummy_on = true;
		out.push_back(f);
	}
	{
		Observation obs = connected_in_call();
		Fixture f = make("settings-encode-changed", "設定: 符号化の設定 (映像エンコーダ、エンコードで優先する項目) を既定値から変えた", obs);
		f.settings.encoder_preference = config::EncoderPreference::Software;
		f.settings.encode_priority = config::EncodePriority::Quality;
		out.push_back(f);
	}
	{
		Observation obs = connected_in_call();
		Fixture f = make("settings-long-edge-custom", "設定: 長い辺の上限に選択肢にない値 (1600) が入っている",
				 obs);
		f.settings.limit_max_long_edge = 1600;
		out.push_back(f);
	}
	{
		Observation obs = connected_in_call();
		obs.install.process = VesktopProcess::NotRunning;
		obs.link.state = LinkState::Disconnected;
		obs.link.disconnected_at_us = kNow - 30 * kSec;
		out.push_back(make("settings-uninstall-ready", "設定: アンインストールのボタンを押せる (配信も録画もしておらず、Vesktop も終了している)", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.uninstall.obs_streaming = true;
		out.push_back(make("settings-uninstall-streaming", "設定: 配信中なので、アンインストールのボタンを押せない", obs));
	}
	{
		// OBS の録画中 (cordmux の録画中は settings-uninstall-recording)。
		Observation obs = connected_in_call();
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.obs_recording = true;
		out.push_back(make("settings-uninstall-obs-recording", "設定: OBS で録画中なので、アンインストールのボタンを押せない", obs));
	}
	{
		// リプレイバッファの動作中と、仮想カメラの動作中。長いほうの理由のテキスト (リプレイバッファ) が、
		// 「バージョン情報」のページのアンインストールのボタンの下で切れないかを見る。
		Observation obs = connected_in_call();
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.obs_replay_buffer = true;
		out.push_back(make("settings-uninstall-replay-buffer", "設定: リプレイバッファの動作中なので、アンインストールのボタンを押せない", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.obs_virtual_camera = true;
		out.push_back(make("settings-uninstall-virtual-camera", "設定: 仮想カメラの動作中なので、アンインストールのボタンを押せない", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.enabled_since_us = kNow - 60 * kSec;
		obs.recording.sender_recording = true;
		out.push_back(make("settings-uninstall-recording", "設定: 録画中なので、アンインストールのボタンを押せない", obs));
	}
	{
		Observation obs = connected_in_call();
		obs.install.process = VesktopProcess::Unknown;
		obs.uninstall.obs_streaming.reset();
		out.push_back(make("settings-uninstall-checking", "設定: まだ確認していないので、アンインストールのボタンを押せない",
				   obs));
	}
	{
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Done;
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.record_dir = "D:\\Videos\\cordmux";
		out.push_back(make("settings-uninstall-done", "設定: アンインストールの後 (項目を操作できず、結果の文が残る)", obs));
	}
	{
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		// 途中で止まったときも、製品は削除するフォルダーを入れる (status-uninstall-stopped のコメント)。
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles, "C:\\Users\\user\\AppData\\Roaming\\obs-studio\\plugin_config\\cordmux"},
					   {UninstallLeftoverKind::Sources, "配信用"},
					   {UninstallLeftoverKind::CollectionNameDuplicate, "配信用"}};
		out.push_back(make("settings-uninstall-stopped", "設定: アンインストールが途中で止まった (残っているものを表示する)", obs));
	}
}

void add_diag_fixtures(std::vector<Fixture> &out)
{
	{
		Observation obs = connected_in_call();
		out.push_back(make("diag-known", "バージョン情報: すべてのバージョンを読めている", obs));
	}
	{
		Observation obs = not_connected();
		obs.versions.gpu.clear();
		obs.versions.bundled_vencord_commit.clear();
		obs.versions.bundled_vencord_tag.clear(); // tag が読めないと「同梱のVencord」は「-」
		obs.install.rewrote_at_startup = true;
		out.push_back(make("diag-unknown", "バージョン情報: 接続していないので、読めないバージョンがある (起動時に書き直した)", obs));
	}
	{
		// 診断情報のエクスポートが済んだ (結果の行と場所の行)。失敗はダイアログで表示する
		// (dialog:export-failed-*)。
		Observation obs = connected_in_call();
		ExportResultFact done;
		done.kind = ExportResultKind::Written;
		done.path = "C:\\Users\\user\\Desktop\\cordmux-diagnostics-20000101-134300.zip";
		obs.ops.export_result = done;
		out.push_back(make("diag-export-done", "バージョン情報: 診断情報をエクスポートした (結果の行と場所)", obs));
	}
	{
		// 同じ結果を、普通の長さの場所 (保存のダイアログの既定値であるデスクトップ。bundle/export_bundle.cpp の
		// default_export_directory) にしたもの。場所の行が列に入らない長さのときの形を見る。
		Observation obs = connected_in_call();
		ExportResultFact done;
		done.kind = ExportResultKind::Written;
		done.path = kUsualDesktopZip;
		obs.ops.export_result = done;
		out.push_back(make("diag-export-done-usual", "バージョン情報: 診断情報をエクスポートした (普通の長さの場所)", obs));
	}
}

void add_dialog_fixtures(std::vector<Fixture> &out)
{
	{
		Fixture f = make("dialog-switch-to-cordmux", "ダイアログ: Vesktop の設定を cordmux に切り替えるかの確認",
				 not_connected());
		f.dialog_vencord = "C:\\Users\\user\\Documents\\Vencord\\dist";
		out.push_back(f);
	}
	{
		Observation obs = connected_in_call();
		obs.install.process = VesktopProcess::NotRunning;
		Fixture f = make("dialog-uninstall-ask", "ダイアログ: アンインストールの確認", obs);
		out.push_back(f);
	}
	{
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Done;
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.record_dir = "D:\\Videos\\cordmux";
		Fixture f = make("dialog-uninstall-done", "ダイアログ: アンインストールの結果 (終わった)", obs);
		out.push_back(f);
	}
	{
		// 手順書と違う置き方。削除するフォルダーが 1 つに決まらないので、folder_to_delete が空
		// (ledger/rules_dock.cpp の loose)。
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Done;
		obs.uninstall.record_dir = "D:\\Videos\\cordmux";
		Fixture f = make("dialog-uninstall-loose", "ダイアログ: アンインストールの結果 (手順書と違う置き方)", obs);
		out.push_back(f);
	}
	{
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		// 途中で止まったときも、製品は削除するフォルダーを入れる (status-uninstall-stopped のコメント)。
		obs.uninstall.folder_to_delete = kFolderToDelete;
		// Vesktop の設定は state.json の場所 (platform/platform_win.cpp の vesktop_state_json_path と同じ形。
		// 名前は架空の user)。場所があると、停止したダイアログの項目ごとの場所の行の 3 つの形 (フォルダー
		// 2 つ、ファイル 1 つ、場所なし) を 1 枚に描画できる。設定のファイルとトークンとポート番号の
		// フォルダーは、1 つの項目の下の 2 行になる (ledger/rules_dock.cpp の settings_places)。
		obs.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles,
					    "C:\\Users\\user\\AppData\\Roaming\\obs-studio\\plugin_config\\cordmux"},
					   {UninstallLeftoverKind::LocalDataFolder, "C:\\Users\\user\\AppData\\Local\\cordmux"},
					   {UninstallLeftoverKind::Sources, "配信用"},
					   {UninstallLeftoverKind::VencordDir, "C:\\Users\\user\\AppData\\Roaming\\Vesktop\\state.json", true}};
		Fixture f = make("dialog-uninstall-stopped", "ダイアログ: アンインストールの結果 (途中で止まった)", obs);
		out.push_back(f);
	}
	{
		// 停止したダイアログの手順書と違う置き方。カードの status-uninstall-stopped-loose と同じ残り。
		// ダイアログの ID は停止したダイアログと同じ (dock/install_dialogs.cpp の
		// uninstall_result_dialog_id) なので、dialog:uninstall-stopped:loose でこの場面を選ぶ
		// (faces.cpp の face_dialog)。
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		obs.uninstall.leftovers = {{UninstallLeftoverKind::Sources, "配信用"},
					   {UninstallLeftoverKind::VencordDir, "C:\\Users\\user\\AppData\\Roaming\\Vesktop\\state.json", true}};
		out.push_back(make("dialog-uninstall-stopped-loose", "ダイアログ: アンインストールの結果 (途中で止まった、手順書と違う置き方)", obs));
	}
	{
		// 3 つのグループがそろった、停止したダイアログ。カードの status-uninstall-stopped-groups と同じ残り。
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles,
					    "C:\\Users\\user\\AppData\\Roaming\\obs-studio\\plugin_config\\cordmux"},
					   {UninstallLeftoverKind::Sources, "配信用"},
					   {UninstallLeftoverKind::VencordDir, "C:\\Users\\user\\AppData\\Roaming\\Vesktop\\state.json", true},
					   {UninstallLeftoverKind::CollectionNotOpened, "配信用 (旧)"}};
		out.push_back(make("dialog-uninstall-stopped-groups", "ダイアログ: アンインストールの結果 (途中で止まった、3 つのグループ)", obs));
	}
	// 製品で起きる、アンインストールが途中で止まったときのダイアログ (カードの
	// status-uninstall-stopped-settings・-not-restored と同じ残り。説明はカードのほうのコメント)。
	// 上の dialog-uninstall-stopped・-loose・-groups は、段階 (1)・(2)・(3) の残りを混ぜた合成の場面で、
	// 製品では同じアンインストールで表示されない。
	// dialog:uninstall-stopped:settings と dialog:uninstall-stopped:not-restored で選ぶ
	// (faces.cpp の face_dialog)。
	{
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.leftovers = {{UninstallLeftoverKind::SettingsFiles,
					    "C:\\Users\\user\\AppData\\Roaming\\obs-studio\\plugin_config\\cordmux\\settings.json", true},
					   {UninstallLeftoverKind::LocalDataFolder, "C:\\Users\\user\\AppData\\Local\\cordmux", false}};
		out.push_back(make("dialog-uninstall-stopped-settings", "ダイアログ: アンインストールの結果 (段階 (1) で止まった。製品で起きる形)", obs));
	}
	{
		Observation obs = not_connected();
		obs.choices.feature = FeatureSwitch::Off;
		obs.install.process = VesktopProcess::NotRunning;
		obs.uninstall.phase = UninstallPhase::Stopped;
		obs.uninstall.folder_to_delete = kFolderToDelete;
		obs.uninstall.settings_dir = kSettingsDirKept;
		obs.uninstall.leftovers = {{UninstallLeftoverKind::VencordDir, "C:\\Users\\user\\AppData\\Roaming\\Vesktop\\state.json", true}};
		out.push_back(make("dialog-uninstall-stopped-not-restored", "ダイアログ: アンインストールの結果 (段階 (3) で止まった、元に戻せなかった設定だけ。製品で起きる形)", obs));
	}
}

// 初めて見る人がひと目で理解できるかを確認するための場面 (glance-)。どれも製品の判断を通す (ほかの場面と同じ)。
// 名前は、ほかの場面にある架空の名前から選ぶ (kenji、ren、yuki、Sakurazaka_Haru1、Sagisawa_Yuika_stream、髙橋 齋藤)。
//
// glance-n1: 一覧についての 4 つの問い (画面共有している人、チェックの意味、チェックがあるのに配信に出ていない人、
//   その人の理由) に答えられるかを見る。枠は 4 つ (config::kScreenSlotCount) なので、チェックの付いた 4 行
//   (表示している・届かなくなった・画質を下げている・表示している) で枠が埋まり、チェックのない 2 行は押せない形に
//   なる (製品が表示する形のまま)。枠を持つのは画面共有している人だけなので、チェックの付いた行は全員が画面共有している。
//   画面共有とチェックが食い違う人は、画面共有しているのにチェックがない Sagisawa_Yuika_stream の 1 人である。
//   チェックがなく画面共有していない人 (髙橋 齋藤) には 2 行目 (声: ...) を付け、目印のない行を作らない。サブアカウントの行は
//   入れない (2 行目のテキストだけが目印なので、問いの「人」と紛れるため)。
// glance-dock-closed: 設定を開く場所を問うための、メニューを閉じたドック (平常。同じ名前の人たち)。
// glance-share-row: 1 人の映像が止まった (行の状態だけで、メッセージにしない)。
// glance-l1-record: メッセージのレベル 1 のメッセージ (録画を開始できなかった)。
// 線形の振れ幅 (dBFS から)。
float peak_of_db(float db)
{
	return std::pow(10.0f, db / 20.0f);
}

// 組の音の大きさ。ミュート中か音量 0 の間 (silenced) は、掛けた後が 0 で、掛ける前の値を灰色で出す。
pipeline::AudioTrackLevel level(int n, pipeline::MixerKind kind, float before_db, float after_db, bool silenced = false)
{
	pipeline::AudioTrackLevel t;
	t.key.user_id = fake_id(n);
	t.key.kind = kind;
	t.peak_before = peak_of_db(before_db);
	t.peak_after = silenced ? 0.0f : peak_of_db(after_db);
	t.silenced = silenced;
	return t;
}

void add_audio_fixtures(std::vector<Fixture> &out)
{
	using pipeline::MixerKind;
	{
		// 音声タブの状態を 1 枚で見る場面。メインアカウントは Taro (声をミュートしたまま話している: 灰色)。
		//   hanako : 声は緑、画面共有の音声は黄 (共有している)
		//   Taro   : メインアカウント。声のミュート中に話している (灰色)
		//   さくら : 声の音量 60%、画面共有の音声をミュート中 (灰色)
		//   kenji  : 共有していない。声が大きい (黄と赤)
		//   ren    : 声の音量 0 (灰色)
		//   mei    : まだ声が届いていない
		//   yuu    : 共有しているが画面共有の音声がない。声は黄
		//   長い名前の人
		Observation obs = connected_in_call();
		obs.choices.composite_present = true;
		obs.people[0].audio.has_screen_audio = true;
		obs.people[1].audio.voice_muted = true;
		obs.people[2].audio.has_screen_audio = true;
		obs.people[2].audio.screen_muted = true;
		obs.people[2].audio.voice_volume_pct = 60;
		obs.people[2].audio.screen_volume_pct = 150;
		obs.people[3].audio.voice_volume_pct = 150;
		PersonFact ren = listening(5, kNameRen);
		ren.audio.voice_volume_pct = 0;
		PersonFact mei = listening(6, "mei");
		mei.voice_arriving = false;
		PersonFact yuu = listening(7, "yuu");
		yuu.sharing = true;
		yuu.sharing_since_us = kNow - 60 * kSec;
		yuu.audio.voice_volume_pct = 120;
		PersonFact long_name = listening(8, kNameLongJa);
		obs.people.push_back(ren);
		obs.people.push_back(mei);
		obs.people.push_back(yuu);
		obs.people.push_back(long_name);
		obs.people.push_back(receiver());
		obs.call.roster_count = 10;
		for (const char *name : {kNameRen, "mei", "yuu", kNameLongJa})
			obs.seen.people.push_back(seen(static_cast<int>(obs.seen.people.size()) + 1, name, 0));
		Fixture f = make("audio-tab", "音声タブ: 話している人、ミュート中に話している人、音量 0、音が大きい人、共有していない人、まだ声が届いていない人", obs);
		f.settings.broadcaster_user_id = fake_id(2);
		f.levels = {
			level(1, MixerKind::Voice, -28, -30),         level(1, MixerKind::ScreenAudio, -20, -16),
			level(2, MixerKind::Voice, -14, 0, true),
			level(3, MixerKind::Voice, -20, -22),         level(3, MixerKind::ScreenAudio, -18, 0, true),
			level(4, MixerKind::Voice, -8, -4),
			level(5, MixerKind::Voice, -25, 0, true),
			level(7, MixerKind::Voice, -20, -17),
			level(8, MixerKind::Voice, -40, -40),
		};
		out.push_back(f);

		// 接続が切れても行を残し、薄くして、操作できなくする。
		Observation off = obs;
		off.last_known_people = off.people;
		off.people.clear();
		off.link.state = ledger::LinkState::Disconnected;
		Fixture g = make("audio-tab-off", "音声タブ: Vesktop との接続が切れた (行は残り、薄くなって操作できない)", off);
		g.settings.broadcaster_user_id = fake_id(2);
		out.push_back(g);

		// 合成のソースがない間は、行を出さずに 1 文を出す。
		Observation none = obs;
		none.choices.composite_present = false;
		Fixture h = make("audio-tab-no-source", "音声タブ: 合成のソースがない (行を出さず、1 文を出す)", none);
		h.settings.broadcaster_user_id = fake_id(2);
		out.push_back(h);

		// 通話にほかの人がいない。
		Observation alone = connected_in_call();
		alone.call.roster_count = 2;
		alone.people = {receiver()};
		out.push_back(make("audio-tab-empty", "音声タブ: 行がない (合成のソースはある)", alone));
	}
	{
		// 名前の長さ違いと、ミュート中のメーター。名前の欄が広がるか (harness-screen-1 が 26 em で省かれない)、省かれた名前が
		// ツールチップで読めるか、ミュート中の行のメーターが音量に合わせて動くかを見る。
		//   hanako           : 短い名前。声は音を出している
		//   harness-screen-1 : 16 文字の名前。声をミュート中で、音量 150% (灰色のメーターが掛ける前より大きく出る)
		//   Al               : 2 文字の名前。画面共有の音声をミュート中で、音量 50% (灰色のメーターが掛ける前より小さく出る)
		//   長い英字、長い日本語 : 名前が省かれる
		Observation obs = connected_in_call();
		obs.people = {showing(1, kNameHanako, 1), listening(2, "harness-screen-1"), listening(3, "Al"),
			      listening(4, kNameLongEn), listening(5, kNameLongJa), listening(6, kNameMika)};
		obs.seen.people.clear();
		for (const char *name : {kNameHanako, "harness-screen-1", "Al", kNameLongEn, kNameLongJa, kNameMika})
			obs.seen.people.push_back(seen(static_cast<int>(obs.seen.people.size()) + 1, name, 0));
		obs.call.roster_count = 7;
		obs.people[0].audio.has_screen_audio = true;
		obs.people[1].audio.voice_muted = true;
		obs.people[1].audio.voice_volume_pct = 150;
		obs.people[2].sharing = true;
		obs.people[2].sharing_since_us = kNow - 60 * kSec;
		obs.people[2].audio.has_screen_audio = true;
		obs.people[2].audio.screen_muted = true;
		obs.people[2].audio.screen_volume_pct = 50;
		Fixture f = make("audio-tab-names", "音声タブ: 名前の長さ違い (16 文字、2 文字、省かれる名前) と、ミュート中のメーター", obs);
		f.levels = {
			level(1, MixerKind::Voice, -28, -28),         level(1, MixerKind::ScreenAudio, -20, -20),
			level(2, MixerKind::Voice, -20, 0, true),
			level(3, MixerKind::ScreenAudio, -12, 0, true),
			level(4, MixerKind::Voice, -24, -24),
			level(5, MixerKind::Voice, -18, -18),
			level(6, MixerKind::Voice, -30, -30),
		};
		out.push_back(f);
	}
	{
		// 人数が多い場面 (30 人)。話している人が半分いる。
		Observation many = thirty_people();
		Fixture f = make("audio-tab-30", "音声タブ: 30 人 (半分が話している)", many);
		for (int i = 0; i < 30; i += 2) {
			f.levels.push_back(level(100 + i, MixerKind::Voice, -30.0f, -26.0f + static_cast<float>(i % 5) * 4.0f));
			if (i < 6)
				f.levels.push_back(level(100 + i, MixerKind::ScreenAudio, -30.0f, -22.0f + static_cast<float>(i)));
		}
		out.push_back(f);
	}
}

// 状態タブ (--tab status で描く)。現在のメッセージ、cordmuxの録画、映像の受信、履歴。累計と履歴と最後の録画は、製品では
// 周期をまたいで DockPoll が持つ状態なので、場面の status に値を入れる。
void add_status_tab_fixtures(std::vector<Fixture> &out)
{
	const auto find_obs = [&out](const char *id) {
		for (const Fixture &f : out) {
			if (f.id == id)
				return f.obs;
		}
		return connected_in_call();
	};
	const auto entry = [](int n, const char *name, std::uint64_t count, std::int64_t total_s) {
		StallEntry e;
		e.user_id = fake_id(n);
		e.name = Label::from_literal(name);
		e.count = count;
		e.total_us = total_s * kSec;
		return e;
	};
	const auto history_line = [](std::int64_t ago_s, const char *id, std::vector<std::string> args = {},
				     std::optional<TextRef> subject = std::nullopt) {
		dockview::StatusHistoryLine line;
		line.when = when_arg_of(kNow - ago_s * kSec, kNow, kJst);
		line.text.id = id;
		line.text.args = std::move(args);
		line.subject = std::move(subject);
		return line;
	};

	// メッセージなし。録画なし。受信の行なし。履歴なし。
	out.push_back(make("status-empty", "状態タブ: メッセージ、録画、受信の行、履歴がどれもない", connected_in_call()));

	// レベル 1 (録画が止まった)、レベル 2 (音声の二重の取り込み、作成の再開)、レベル 3 (録画ファイルの作成の進み具合)。
	{
		Observation obs = connected_in_call();
		obs.recording.stopped_by_space_exhausted = true;
		obs.choices.discord_audio_source = DiscordAudioSource::Found;
		obs.choices.discord_audio_source_name = "アプリケーション音声キャプチャ (ベータ版)";
		obs.recording.pending_present = true;
		obs.recording.pending_session_count = 1;
		obs.recording.finalize = FinalizeState::Running;
		obs.recording.finalize_files_done = 3;
		obs.recording.finalize_files_total = 8;
		obs.recording.finalize_current_index = 4;
		obs.recording.finalize_permille = 420;
		obs.recording.phase_since_us = kNow - 5 * 60 * kSec;
		obs.recording.finalize_last_progress_us = kNow - kSec;
		out.push_back(make("status-messages", "状態タブ: メッセージのレベル 1、2、3 と、操作のボタン", obs));
	}

	// 録画ファイルの作成の進み具合 (レベル 3)。
	out.push_back(make("status-finalize", "状態タブ: 録画ファイルの作成の進み具合 (メッセージのレベル 3)",
			   find_obs("notice-record-finalize-percent")));

	// 録画中。
	{
		Observation obs = connected_in_call();
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.phase_since_us = kNow - 12 * 60 * kSec;
		obs.recording.enabled_since_us = kNow - 12 * 60 * kSec;
		obs.recording.sender_recording = true;
		obs.recording.remaining_seconds = 5 * 3600;
		Fixture f = make("status-recording", "状態タブ: cordmuxの録画が録画中", obs);
		f.status.recording.active = true;
		f.status.recording.start_us = kNow - 12 * 60 * kSec;
		out.push_back(f);
	}

	// 最後の録画。
	{
		Fixture f = make("status-last-recording", "状態タブ: 録画していない。最後の録画の時刻と長さと [フォルダーを開く]",
				 connected_in_call());
		f.status.recording.has_last = true;
		f.status.recording.last_start_us = kNow - 55 * 60 * kSec;
		f.status.recording.last_duration_us = 12 * 60 * kSec;
		f.status.recording.last_folder = std::string(kUsualRecordDir) + "\\2000-01-01 12-48-00";
		out.push_back(f);
	}

	// 映像の受信の表。3 人の直近 1 分と累計、通話を抜けた人の累計だけの行、長い名前。
	{
		Observation obs = connected_in_call();
		obs.people.push_back(showing(5, kNameLongJa, 4));
		obs.seen.people.push_back(seen(5, kNameLongJa, 0));
		obs.receive.arrival = sample_arrival_facts(1, 3);
		Fixture f = make("status-table", "状態タブ: 映像の受信の表 (直近 1 分と累計。通話を抜けた人の行と長い名前を含む)", obs);
		f.status.tally.entries = {entry(1, kNameHanako, 3, 11), entry(2, kNameTaro, 0, 0), entry(9, "guest9", 12, 1840)};
		f.status.tally.entries.erase(f.status.tally.entries.begin() + 1);
		out.push_back(f);
	}

	// 映像の受信の表。30 人の直近 1 分と、途切れの累計がある人。
	{
		Observation obs = thirty_people();
		obs.receive.arrival = sample_arrival_facts(100, 30);
		Fixture f = make("status-table-30", "状態タブ: 映像の受信の表が 30 人 (途切れの累計がある人を含む)", obs);
		f.status.tally.entries = {entry(100, "guest1", 3, 11), entry(105, "guest6", 20, 140), entry(120, "guest21", 1, 2)};
		out.push_back(f);
	}

	// 合成のソースがない。
	{
		Observation obs = connected_in_call();
		obs.choices.composite_present = false;
		out.push_back(make("status-no-composite", "状態タブ: cordmuxのソースがない (表の代わりに 1 文)", obs));
	}

	// 履歴が上限の 60 行。
	{
		Fixture f = make("status-history-60", "状態タブ: 履歴が上限の 60 行", connected_in_call());
		const TextRef subjects[] = {TextRef{"cordmux.dock.vesktop_not_connected.headline", {}},
					    TextRef{"cordmux.dock.double_audio.headline", {"アプリケーション音声キャプチャ"}}};
		for (int i = 0; i < 60; ++i) {
			const std::int64_t ago = 30 + i * 53;
			switch (i % 6) {
			case 0:
				f.status.history.push_back(history_line(ago, keys::kStatusHistoryNoticeAppearedStatus, {},
									subjects[(i / 6) % 2]));
				break;
			case 1:
				f.status.history.push_back(history_line(ago, keys::kStatusHistoryNoticeClearedStatus, {},
									subjects[(i / 6) % 2]));
				break;
			case 2:
				f.status.history.push_back(history_line(ago, keys::kStatusHistorySlotAddedStatus, {kNameTaro}));
				break;
			case 3:
				f.status.history.push_back(history_line(ago, keys::kStatusHistoryDisconnectedStatus));
				break;
			case 4:
				f.status.history.push_back(history_line(ago, keys::kStatusHistoryConnectedStatus));
				break;
			default:
				f.status.history.push_back(history_line(ago, i % 12 == 5 ? keys::kStatusHistoryRecordingStartedStatus
											   : keys::kStatusHistoryRecordingStoppedStatus));
				break;
			}
		}
		out.push_back(f);
	}

	// 映像が届かなくなり、受信をやり直した (新しい順。やり直しの前に、枠に載せた行と接続の行がある)。
	{
		Fixture f = make("status-history-restart", "状態タブ: 映像が届かなくなり、受信をやり直した", connected_in_call());
		f.status.history.push_back(history_line(40, keys::kStatusHistoryReceiveRestartedStatus, {kNameHanako}));
		f.status.history.push_back(history_line(400, keys::kStatusHistorySlotAddedStatus, {kNameHanako}));
		f.status.history.push_back(history_line(460, keys::kStatusHistoryConnectedStatus));
		out.push_back(f);
	}
}

void add_display_fixtures(std::vector<Fixture> &out)
{
	// 表示タブ。設定の値だけを替えた場面で、メインアカウントがいる通話 (名前ラベルと、枠の並びを確認する場面) を使う。
	// 入力欄を押せなくする条件と、ソースを選んだ形 (候補は render.cpp の build_dock が渡す) を見る。
	Observation obs = connected_in_call();
	obs.choices.composite_present = true;
	const auto add = [&](const char *id, const char *note, const std::function<void(Fixture &)> &change) {
		Fixture f = make(id, note, obs);
		f.settings.broadcaster_user_id = fake_id(2);
		if (change)
			change(f);
		out.push_back(std::move(f));
	};
	add("display-default", "表示タブ: 既定値 (メインアカウントがある)", nullptr);
	add("display-no-main", "表示タブ: メインアカウントがない (自分の枠に映す映像の先頭は (なし))",
	    [](Fixture &f) { f.settings.broadcaster_user_id.clear(); });
	add("display-dummy", "表示タブ: ダミー映像を表示している (1 行目は「ダミー映像を表示中」)",
	    [](Fixture &f) { f.dummy_on = true; });
	add("display-order-added", "表示タブ: 枠に載せる人の選び方が自動で、並び順が載せた順", [](Fixture &f) {
		f.settings.assign_mode = config::AssignMode::Auto;
		f.settings.slot_order = config::SlotOrder::Added;
		f.settings.tile_gap_px = 24;
	});
	add("display-self-frame-chosen", "表示タブ: 自分の枠に映す映像を選んだ", [](Fixture &f) {
		f.settings.self_frame_source_uuid = "preview-capture";
		f.settings.self_frame_source_name = "Video Capture Device";
	});
	add("display-self-frame-missing", "表示タブ: 保存した映像が OBS に見つからない", [](Fixture &f) {
		f.settings.self_frame_source_uuid = "preview-gone";
		f.settings.self_frame_source_name = "HDMI Capture";
	});
	add("display-name-off", "表示タブ: 名前の [表示] を切った (フォント、色、輪郭が押せない)",
	    [](Fixture &f) { f.settings.label_show_name = false; });
	add("display-name-avatar-off", "表示タブ: 名前とアバターの [表示] を切った (背景も押せない)", [](Fixture &f) {
		f.settings.label_show_name = false;
		f.settings.label_show_avatar = false;
	});
	add("display-outline-off", "表示タブ: 輪郭の [表示] を切った (輪郭の色とサイズが押せない)",
	    [](Fixture &f) { f.settings.label_show_outline = false; });
	add("display-plate-off", "表示タブ: 名前ラベルの背景の [表示] を切った (色と不透明度が押せない)",
	    [](Fixture &f) { f.settings.label_show_plate = false; });
	add("display-slot-background-off", "表示タブ: 枠の背景の [表示] を切った (色と不透明度が押せず、値は残る)", [](Fixture &f) {
		f.settings.slot_background_show = false;
		f.settings.slot_background_color = "#336699";
		f.settings.slot_background_opacity_pct = 60;
	});
	add("display-slot-label-off",
	    "表示タブ: 画面共有している参加者の [表示] を切った ([1人のときは非表示]、位置、アバターの位置が押せない)",
	    [](Fixture &f) { f.settings.label_show_in_slot = false; });
	add("display-glow-off", "表示タブ: [光] を切った (暗さがあるので、時間は押せる)",
	    [](Fixture &f) { f.settings.label_show_speaking = false; });
	add("display-glow-off-dim-0", "表示タブ: [光] を切り、暗さも 0 (光の形と時間が押せない)", [](Fixture &f) {
		f.settings.label_show_speaking = false;
		f.settings.label_dim_pct = 0;
	});
	add("display-roster-off", "表示タブ: 画面共有していない参加者の [表示] を切った",
	    [](Fixture &f) { f.settings.roster_show = false; });
	add("display-roster-top-center", "表示タブ: 画面共有していない参加者の配置が中央上 (並びは横並びだけ)", [](Fixture &f) {
		f.settings.roster_anchor = config::RosterAnchor::TopCenter;
		f.settings.roster_direction = config::RosterDirection::Row;
	});
	add("display-weight-off-list", "表示タブ: 太さが選択肢にない値 (550)", [](Fixture &f) { f.settings.label_font_weight = 550; });
	add("display-self-source-chosen", "表示タブ: 自分の枠の音声ソースを選んだ (しきい値が押せる)", [](Fixture &f) {
		f.settings.self_speaking_source_uuid = "preview-mic";
		f.settings.self_speaking_source_name = "Mic/Aux";
	});
	add("display-self-source-missing", "表示タブ: 保存した音声ソースが OBS に見つからない", [](Fixture &f) {
		f.settings.self_speaking_source_uuid = "preview-gone";
		f.settings.self_speaking_source_name = "USB Mic";
	});
	add("display-long-values", "表示タブ: 長い名前のフォントと、大きな値", [](Fixture &f) {
		f.settings.label_font_family = "BIZ UDPGothic";
		f.settings.label_em_ratio_pct = 8.5;
		f.settings.label_margin_pct = 13.9;
		f.settings.label_border_rise_ms = 5000;
		f.settings.label_border_hold_ms = 5000;
		f.settings.label_border_fade_ms = 5000;
		f.settings.self_speaking_threshold_db = -96;
	});
}

void add_glance_fixtures(std::vector<Fixture> &out)
{
	const char *const kSakurazaka = "Sakurazaka_Haru1";
	const char *const kSagisawa = "Sagisawa_Yuika_stream";
	const char *const kYuki = "yuki";
	const char *const kTakahashi = "髙橋 齋藤";
	{
		Observation obs = connected_in_call();
		obs.call.roster_count = 6;
		// 表示している。
		PersonFact shown = showing(21, kNameKenji, 1);
		// 届かなくなった (届いていたフレームが、止まったと判断するしきい値を超えて届かない)。
		PersonFact stalled = showing(22, kNameRen, 2);
		stalled.last_video_frame_at_us = kNow - 20 * kSec;
		// 画質を下げている (配信には出ている)。
		PersonFact reduced = showing(23, kSakurazaka, 3, 960, 540, 30.0);
		reduced.stage = std::string(kStageDownscaled);
		// 表示している (枠を持つのは画面共有している人だけ)。
		PersonFact steady = showing(24, kYuki, 4);
		// 画面共有しているのにチェックがない。
		PersonFact unchecked_sharing = listening(25, kSagisawa);
		unchecked_sharing.sharing = true;
		unchecked_sharing.sharing_since_us = kNow - 120 * kSec;
		// チェックがなく共有していない (2 行目に声: kenji)。
		PersonFact quiet = listening(26, kTakahashi);
		// 並びは通話の参加者の順 (チェックの有無でまとめない。上の 4 行がチェックのある行、とならないように)。
		obs.people = {unchecked_sharing, shown, quiet, stalled, steady, reduced};
		obs.seen.people = {seen(25, kSagisawa, 0), seen(21, kNameKenji, 0), seen(26, kTakahashi, 0),
				   seen(22, kNameRen, 0), seen(24, kYuki, 0), seen(23, kSakurazaka, 0)};
		obs.seen.people[2].voice_source_user_id = fake_id(21);
		out.push_back(make("glance-n1", "ひと目での確認: 画面共有とチェックが食い違う一覧 (表示している・届かなくなった・画質・チェックがない)", obs));
	}
	const auto healthy = [&]() {
		Observation obs = connected_in_call();
		obs.call.roster_count = 5;
		obs.people = {showing(21, kNameKenji, 1), showing(22, kNameRen, 2, 1920, 1080, 60.0),
			      showing(23, kSakurazaka, 3), listening(24, kYuki), listening(25, kSagisawa)};
		obs.seen.people = {seen(21, kNameKenji, 0), seen(22, kNameRen, 0), seen(23, kSakurazaka, 0),
				   seen(24, kYuki, 0), seen(25, kSagisawa, 0)};
		return obs;
	};
	out.push_back(make("glance-dock-closed", "ひと目での確認: 平常のドック (設定を開く場所の問い。メニューは閉じている)", healthy()));
	{
		Observation obs = healthy();
		obs.people[1].last_video_frame_at_us = kNow - 20 * kSec;
		out.push_back(make("glance-share-row", "ひと目での確認: 1 人の映像が止まった (行の状態だけで、メッセージにしない)", obs));
	}
	{
		// notice-record-dir-unwritable と同じ形 (始めようとして書き込めなかった。メッセージのレベル 1)。
		Observation obs = healthy();
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.save_dir_writable = false;
		obs.recording.phase = RecordPhase::StartFailed;
		obs.recording.phase_since_us = kNow - 3 * kSec;
		obs.recording.unwritable_dir = "D:\\Videos\\cordmux";
		out.push_back(make("glance-l1-record", "ひと目での確認: メッセージのレベル 1 (cordmuxの録画を開始できなかった。保存先に書き込めない)", obs));
	}
}

} // namespace

std::vector<Fixture> all_fixtures()
{
	std::vector<Fixture> out;
	add_status_fixtures(out);
	add_record_fixtures(out);
	add_dock_extra_fixtures(out);
	add_people_fixtures(out);
	add_audio_fixtures(out);
	add_display_fixtures(out);
	add_status_tab_fixtures(out);
	add_name_fixtures(out);
	add_seen_fixtures(out);
	add_settings_fixtures(out);
	add_diag_fixtures(out);
	add_dialog_fixtures(out);
	add_glance_fixtures(out);
	return out;
}

std::vector<Fixture> dock_fixtures()
{
	// ドックに描画する場面は、状態の行 (status-)・メッセージ (notice-)・参加者の一覧 (people- と rows-)・音声タブ
	// (audio-)・表示タブ (display-)・録画のボタン (record-)・操作の失敗の行 (added-)・ひと目での確認 (glance-)。ほかはウィンドウの場面
	// (seen- と settings- は参加者の設定ウィンドウと設定ウィンドウ、diag- は「バージョン情報」、dialog- は確認と
	// 結果のダイアログ) で、--face settings、participants、dialog が使う。
	std::vector<Fixture> out;
	for (Fixture &f : all_fixtures()) {
		const std::string &id = f.id;
		if (id.rfind("status-", 0) == 0 || id.rfind("notice-", 0) == 0 || id.rfind("people-", 0) == 0 ||
		    id.rfind("rows-", 0) == 0 || id.rfind("record-", 0) == 0 || id.rfind("added-", 0) == 0 ||
		    id.rfind("glance-", 0) == 0 || id.rfind("audio-", 0) == 0 || id.rfind("display-", 0) == 0)
			out.push_back(std::move(f));
	}
	return out;
}

std::vector<TimelineStep> timeline_steps()
{
	// 録画中の問題のない状態から始め、残りの時間がしきい値を下回る (メッセージのレベル 2。表示する前の待ちがある)、
	// 音声の二重の取り込み (メッセージのレベル 2。待ちなし)、ボタンを押している間に枠がなくなる (メッセージのレベル 2。
	// 位置の変化は並びの保留でためる)、ボタンを離す、残りの時間が戻る (消す前の待ち) の順に進める。
	const auto at = [](std::int64_t t_us) {
		Observation obs = connected_in_call();
		obs.now_us = kNow + t_us;
		obs.choices.recording = FeatureSwitch::On;
		obs.recording.phase = RecordPhase::Recording;
		obs.recording.phase_since_us = kNow - 600 * kSec;
		obs.recording.enabled_since_us = kNow - 600 * kSec;
		obs.recording.sender_recording = true;
		obs.recording.remaining_seconds = 5 * 3600;
		for (PersonFact &p : obs.people) {
			if (p.last_video_frame_at_us.has_value())
				p.last_video_frame_at_us = obs.now_us - kSec / 2;
		}
		return obs;
	};
	const auto low = [](Observation obs) {
		obs.recording.remaining_seconds = 14 * 60 + 30;
		return obs;
	};
	const auto dbl = [](Observation obs) {
		obs.choices.discord_audio_source = DiscordAudioSource::Found;
		obs.choices.discord_audio_source_name = "アプリケーション音声キャプチャ (ベータ版)";
		return obs;
	};
	const auto nocomposite = [](Observation obs) {
		obs.choices.composite_present = false;
		return obs;
	};
	std::vector<TimelineStep> out;
	out.push_back({"t00-0.0s", "録画中、何も起きていない", at(0), false});
	out.push_back({"t01-0.5s", "残りの時間がしきい値を下回った (メッセージのレベル 2。表示する前の待ちの最中なので、まだ表示しない)", low(at(kSec / 2)), false});
	out.push_back({"t02-2.6s", "待ちが済んで、メッセージのレベル 2 が表示された", low(at(2600 * kSec / 1000)), false});
	out.push_back({"t03-3.0s", "音声の二重の取り込みのメッセージが表示された (待ちなし。表示した順に下へ追加される)", dbl(low(at(3 * kSec))), false});
	out.push_back({"t04-3.5s", "ボタンを押している間に cordmux のソースがなくなった (位置が動く変化は並びの保留でためる)", nocomposite(dbl(low(at(3500 * kSec / 1000)))), true});
	out.push_back({"t05-4.0s", "ボタンを離した (並びの保留でためていた変化を反映する)", nocomposite(dbl(low(at(4 * kSec)))), false});
	out.push_back({"t06-5.0s", "残りの時間が戻った (消す前の待ちの最中なので、まだ消えない)", nocomposite(dbl(at(5 * kSec))), false});
	out.push_back({"t07-6.1s", "待ちが済んで、メッセージのレベル 2 が消えた", nocomposite(dbl(at(6100 * kSec / 1000))), false});
	return out;
}

Fixture fixture_by_id(const std::string &id)
{
	for (Fixture &f : all_fixtures()) {
		if (f.id == id)
			return f;
	}
	return Fixture{};
}

namespace {

// 待ちが済んだ View。1 行目は既読を見ていない形のままである。
ledger::dockview::DockViewV2 stable_view_for(const Fixture &fixture)
{
	const Observation obs = observation_for(fixture);
	const ledger::dockview::DockViewV2 raw = decide_dock(obs);
	NoticeTrackerConfig cfg;
	cfg.show_delay_us = static_cast<std::int64_t>(fixture.settings.notice_show_delay_ms) * 1000;
	cfg.clear_delay_us = static_cast<std::int64_t>(fixture.settings.notice_clear_delay_ms) * 1000;
	// 製品の DockPoll と同じく、状態を引き継いで 2 回通す (1 回目で待ちが始まり、2 回目で待ちが済む)。
	const StabilizeResult first = stabilize(NoticeTrackerState{}, raw, obs.now_us - cfg.show_delay_us - 1, cfg);
	const StabilizeResult second = stabilize(first.next, raw, obs.now_us, cfg);
	return second.view;
}

} // namespace

ledger::FirstLineResult first_line_for(const Fixture &fixture, bool status_tab_open)
{
	const Observation obs = observation_for(fixture);
	FirstLineInput input;
	input.now_us = obs.now_us;
	input.streaming = obs.obs_outputs.streaming.value_or(false);
	input.status_tab_open = status_tab_open;
	return settle_first_line(NoticeReadState{}, stable_view_for(fixture), input);
}

ledger::dockview::DockViewV2 view_for(const Fixture &fixture)
{
	ledger::dockview::DockViewV2 view = stable_view_for(fixture);
	view.first_line = first_line_for(fixture, false).first_line;
	// 状態タブ。製品の DockPoll と同じく、待ちが済んだ View と Observation から決める。履歴を渡した場面は、今あるメッセージを
	// 発生として履歴に足さない。
	const Observation obs = observation_for(fixture);
	StatusState state = fixture.status;
	if (!state.history.empty())
		state.prev = history_snapshot_of(view, obs);
	StatusInput input;
	input.now_us = obs.now_us;
	input.utc_offset_sec = obs.utc_offset_sec;
	input.streaming = state.streaming;
	view.status_tab = settle_status(state, view, obs, input).view;
	return view;
}

std::vector<ledger::ArrivalFact> sample_arrival_facts(int first_n, int count)
{
	struct Spec {
		int base_ms;
		int jitter_ms;
		int burst_every;
		int burst_ms;
	};
	const Spec specs[] = {{18, 10, 700, 160}, {60, 70, 40, 130}, {25, 12, 0, 0}, {30, 20, 300, 120}};
	unsigned seed = 12345u;
	const auto next = [&seed]() {
		seed = seed * 1664525u + 1013904223u;
		return seed >> 8;
	};
	std::vector<ledger::ArrivalFact> out;
	for (int i = 0; i < count; ++i) {
		const Spec &spec = specs[i % 4];
		pipeline::ArrivalWindow window;
		double sum = 0.0;
		for (int k = 0; k < 3600; ++k) {
			double ms = spec.base_ms + static_cast<double>(next() % static_cast<unsigned>(spec.jitter_ms + 1));
			if (spec.burst_every > 0 && k % spec.burst_every < 6)
				ms += spec.burst_ms + static_cast<double>(next() % 60u);
			window.delays_ms.push_back(static_cast<std::uint16_t>(ms));
			sum += ms;
			window.max_ms = std::max(window.max_ms, ms);
		}
		window.mean_ms = sum / 3600.0;
		ledger::ArrivalFact fact;
		fact.user_id = fake_id(first_n + i);
		fact.streams.push_back(std::move(window));
		out.push_back(std::move(fact));
	}
	return out;
}

ledger::Observation observation_for(const Fixture &fixture)
{
	ledger::Observation obs = fixture.obs;
	const config::Settings &s = fixture.settings;
	obs.choices.dummy = fixture.dummy_on ? FeatureSwitch::On : FeatureSwitch::Off;
	obs.choices.broadcaster_user_id = s.broadcaster_user_id;
	obs.choices.assign_mode = s.assign_mode == config::AssignMode::Auto ? AssignMode::Auto : AssignMode::Manual;
	obs.choices.record_dir = s.record_dir;
	obs.choices.record_output_pcm = s.record_output_pcm == config::RecordPcmFormat::Int24 ? RecordPcm::Int24
											       : RecordPcm::Float32;
	obs.choices.limit_max_long_edge = s.limit_max_long_edge;
	obs.choices.limit_fps_divisor = s.limit_fps_divisor;
	// 枠に載っていて画面共有している人は、製品では枠に映る (dock/build_observation.cpp の slot_visible)。
	for (ledger::PersonFact &person : obs.people)
		person.slot_visible = person.slot.has_value() && (person.sharing || !person.sharing_known);
	// 自分の枠の人は、製品ではメインアカウントで、設定がなければ Vesktop のアカウントである (dock/build_observation.cpp の
	// self_frame_user_id)。
	for (ledger::PersonFact &person : obs.people) {
		if (s.broadcaster_user_id.empty() ? person.is_receiver : person.user_id == s.broadcaster_user_id)
			person.is_self_frame = true;
	}
	// 自分の枠の人の user_id と名前 (製品の dock/build_observation.h の self_frame_identity)。場面で決めていなければ、自分の枠の人
	// の行から入れる。行の枠の位置と配信の配置が、この同じ値を使う。
	if (obs.choices.self_frame_user_id.empty()) {
		for (const ledger::PersonFact &person : obs.people) {
			if (person.is_self_frame) {
				obs.choices.self_frame_user_id = person.user_id;
				obs.choices.self_frame_name = person.display_name.value_or(std::string());
				break;
			}
		}
	}
	// 全画面にできる人と、全画面の人の配信に出している名前 (製品の dock/dock_poll.cpp の current_facts)。
	obs.choices.focus_candidates.clear();
	obs.choices.focus_name.clear();
	for (const sources::CompositeFocusTile &tile : focus_tiles_for(obs)) {
		obs.choices.focus_candidates.push_back(tile.user_id);
		if (!obs.choices.focus_user_id.empty() && tile.user_id == obs.choices.focus_user_id &&
		    obs.choices.focus_name.empty())
			obs.choices.focus_name = tile.name;
	}
	return obs;
}

std::vector<sources::CompositeFocusTile> focus_tiles_for(const ledger::Observation &obs)
{
	const ledger::Choices &c = obs.choices;
	if (c.dummy == FeatureSwitch::On)
		return {};
	sources::CompositeInput input;
	input.self_shown = c.self_frame_shown;
	input.full_when_single = ledger::kFullWhenSingle;
	input.slot_order = c.slot_order;
	input.self_first = c.self_slot_first;
	input.self_added_mark = c.self_frame_added_mark;
	for (const ledger::PersonFact &p : obs.people) {
		if (!p.slot.has_value() || (p.sharing_known && !p.sharing))
			continue;
		sources::CompositeShare share;
		share.slot = *p.slot;
		share.name = p.display_name.value_or(std::string());
		share.user_id = p.user_id;
		share.added_order = p.slot_added_order;
		share.is_main = !c.broadcaster_user_id.empty() && p.user_id == c.broadcaster_user_id;
		input.shares.push_back(std::move(share));
	}
	// 自分の枠の人は、行の枠の位置と同じ値 (製品の dock/build_observation.h の self_frame_identity)。
	input.self_user_id = c.self_frame_user_id;
	input.self_name = c.self_frame_name;
	return sources::composite_focus_tiles(input);
}

std::vector<std::string> uncovered_rules(const std::vector<Fixture> &fixtures)
{
	std::set<std::size_t> primary_hit;
	std::set<std::size_t> notice_hit;
	for (const Fixture &fixture : fixtures) {
		const Observation obs = observation_for(fixture);
		const PrimaryRuleId primary = primary_rule_of(obs);
		for (std::size_t i = 0; i < primary_rule_count(); ++i) {
			if (primary_rule_id_at(i) == primary)
				primary_hit.insert(i);
		}
		for (std::size_t i = 0; i < notice_rule_count(); ++i) {
			if (notice_rule_fires(notice_rule_id_at(i), obs))
				notice_hit.insert(i);
		}
	}
	// decide_dock だけが使う行 (AddedRuleId)。メッセージの rule_name (列挙子の名前) で数える。
	std::set<std::string> added_hit;
	for (const Fixture &fixture : fixtures) {
		for (const dockview::Notice &n : decide_dock(observation_for(fixture)).notices) {
			added_hit.insert(n.rule_name);
			// ReinstallFailed は、同じ見出しのカード (上書きされています) があれば、そのカードに段落を
			// 追加して 2 枚にしない (ledger/rules_dock.cpp の ReinstallFailed)。段落のテキストが
			// 表示されていれば、成り立ったと数える。
			for (const auto &body : n.body) {
				if (body.id == text::keys::kDockReinstallFailedText)
					added_hit.insert("ReinstallFailed");
			}
		}
	}
	std::vector<std::string> missing;
	for (std::size_t i = 0; i < added_rule_count(); ++i) {
		const std::string name = rule_log_name(added_rule_id_at(i));
		if (added_hit.count(name) == 0)
			missing.push_back("added rule " + name);
	}
	// notices の表の行が、decide_dock で 1 回以上メッセージになっているかも見る (表の条件が成り立っていても、
	// decide_dock が別の条件で表示しない行がある。例: DoubleAudio は、ソースの名前がないと表示しない)。
	for (std::size_t i = 0; i < notice_rule_count(); ++i) {
		const std::string name = rule_log_name(notice_rule_id_at(i));
		if (added_hit.count(name) == 0)
			missing.push_back("notice rule " + name + " (decide_dock)");
	}
	for (std::size_t i = 0; i < primary_rule_count(); ++i) {
		if (primary_hit.count(i) == 0)
			missing.push_back("primary rule #" + std::to_string(i));
	}
	for (std::size_t i = 0; i < notice_rule_count(); ++i) {
		if (notice_hit.count(i) == 0)
			missing.push_back("notice rule #" + std::to_string(i));
	}
	return missing;
}

} // namespace cordmux::ui_preview
