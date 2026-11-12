// SPDX-License-Identifier: Apache-2.0

// DockPoll の処理のうち、ポーリング、描画、押された操作の実行と、録画、ダミー、枠の割り当て。ほかの処理は
// dock_poll_layout.cpp、dock_poll_install.cpp、dock_poll_settings.cpp、dock_poll_export.cpp にある (dock_poll.h の
// friend の宣言)。

#include "dock/dock_poll.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QDateTime>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "config/allocation.h"
#include "config/seen_people.h"
#include "diag/diag_log.h"
#include "dock/dock_poll_internal.h"
#include "dock/install_flow.h"
#include "dock/people_list.h"
#include "install/uninstall.h"
#include "dock/ui/open_location.h"
#include "dock/ui/text_resolve.h"
#include "ledger/look_tab.h"
#include "ledger/notice_tracker.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"
#include "platform/platform.h"
#include "sources/composite_source.h"
#include "sources/double_audio.h"
#include "sources/dummy_feed.h"
#include "sources/self_frame_source.h"
#include "sources/self_speaking_source.h"

namespace cordmux::dock {

using detail::wall_now_us;

namespace {

// マイクの光と音声タブのメーターの値を読む間隔 (ms)。Pipeline::audio_levels は 1 秒に 10 回呼んでよい軽さで作ってある。
constexpr int kLevelPollMs = 100;

// ---- 設定ウィンドウの「バージョン情報」のページに表示する、OBS のバージョンと GPU の名前 ----
//
// どれも、読めなければ空を返す。読めなかったことを推測した値で埋めない。画面の "-" は ledger::decide_dock が入れる。

std::string obs_version_text()
{
	const char *version = obs_get_version_string();
	return version != nullptr ? std::string(version) : std::string();
}

// 描画に使うデバイス (GPU) の名前。描画のコンテキストの中でしか読めないので、obs_enter_graphics と
// obs_leave_graphics で挟む。起動中に変わらない値なので、読むのは 1 回だけである。ポーリングのたびにコンテキストを
// 取ると、周期ごとに描画スレッドを待たせることになる。
std::string gpu_name_text()
{
	obs_enter_graphics();
	const char *name = gs_get_device_name();
	std::string out = name != nullptr ? std::string(name) : std::string();
	obs_leave_graphics();
	return out;
}

// ログに書く操作の名前。画面には表示しないので ini を使わない。押されたが実行しなかった操作を、
// ログから読めるようにするための文字列である。OBS のログは英語で書き、列挙子の名前は書かないので、英語の画面のボタンと
// メニューのテキストと同じ語にする。ボタンのない操作は、何をする操作かを短く書く。
const char *action_name(ledger::ActionId id)
{
	switch (id) {
	case ledger::ActionId::WriteVencordDir:
		return "Add to Vesktop";
	case ledger::ActionId::RepairVencordDir:
		return "Add to Vesktop (fix)";
	case ledger::ActionId::ReinstallVencordFiles:
		return "Reinstall";
	case ledger::ActionId::ExportBundle:
		return "Export Diagnostics";
	case ledger::ActionId::SetAlwaysOn:
		return "Add Automatically";
	case ledger::ActionId::SetSlot:
		return "slot change";
	case ledger::ActionId::SetVoiceSource:
		return "Voice From";
	case ledger::ActionId::SetNameOverride:
		return "Rename";
	case ledger::ActionId::ToggleFeature:
		return "Enable or Disable cordmux";
	case ledger::ActionId::ToggleRecording:
		return "Start or Stop cordmux Recording";
	case ledger::ActionId::ToggleDummy:
		return "Show placeholders";
	case ledger::ActionId::ResumeFinalize:
		return "Resume Finalizing";
	case ledger::ActionId::Uninstall:
		return "Uninstall cordmux";
	case ledger::ActionId::OpenSettings:
		return "Open cordmux Settings";
	case ledger::ActionId::OpenParticipants:
		return "Participant Settings";
	case ledger::ActionId::SetRosterShown:
		return "Show";
	case ledger::ActionId::SetVoiceMute:
		return "Mute Voice";
	case ledger::ActionId::SetScreenAudioMute:
		return "Mute Screen Audio";
	case ledger::ActionId::SetVoiceVolume:
		return "Voice Volume";
	case ledger::ActionId::SetScreenAudioVolume:
		return "Screen Audio Volume";
	case ledger::ActionId::SetSelfFrame:
		return "self frame change";
	case ledger::ActionId::MoveToMainCall:
		return "Move";
	case ledger::ActionId::LeaveCall:
		return "Leave";
	case ledger::ActionId::SetMainAccount:
		return "Change";
	case ledger::ActionId::ReplaceSlot:
		return "Replace";
	case ledger::ActionId::ResetStallTally:
		return "Reset Stall Tally";
	case ledger::ActionId::SetFocus:
		return "Show Full Screen or Restore";
	case ledger::ActionId::RemoveFromVesktop:
		return "Remove from Vesktop";
	}
	return "unknown";
}

int log_level_of(ledger::dockview::LogLevel level)
{
	switch (level) {
	case ledger::dockview::LogLevel::Error:
		return LOG_ERROR;
	case ledger::dockview::LogLevel::Warning:
		return LOG_WARNING;
	case ledger::dockview::LogLevel::Info:
		return LOG_INFO;
	}
	return LOG_INFO;
}

const char *tier_log_name(const ledger::NoticeEvent &e)
{
	if (e.is_row)
		return "row";
	switch (e.tier) {
	case ledger::dockview::Tier::L1:
		return "1";
	case ledger::dockview::Tier::L2:
		return "2";
	case ledger::dockview::Tier::L3:
		return "3";
	}
	return "3";
}

// メッセージに表示した場所を開く (場所の行にあるボタンから呼ぶ)。開く関数はダイアログと同じ 2 つで、どこを開くか
// (もうない場所なら、存在する中でいちばん近い親フォルダー) は、その関数が決める。
//   - [ファイルの場所を開く] (ファイルの行): platform::open_file_location。エクスプローラーで、そのファイルを選択した
//     状態で開く。結果のダイアログと同じ関数である。
//   - [フォルダーを開く] (フォルダーの行): ui::open_folder。確認と結果のダイアログ、設定ウィンドウと同じ関数である。
//     存在するフォルダーはその中を、存在するファイルはそれを含むフォルダーを、もうない場所は存在する中で
//     いちばん近い親フォルダーを開く。
// フォルダーの行を platform::open_file_location に渡さないのは、存在するフォルダーの親を開いてそのフォルダーを
// 選択する動作になり、ボタンのテキスト「フォルダーを開く」と違うためである。開く場所の規則をここやメッセージのカードの中に
// 持たないのは、ダイアログと規則が 2 か所に分かれないようにするためである。
void open_location_of(const std::string &path_utf8, ledger::dockview::LocationKind kind)
{
	if (kind == ledger::dockview::LocationKind::File) {
		platform::open_file_location(path_utf8);
		return;
	}
	// 開けなかったとき (場所が空のときや、ルートまでさかのぼっても存在する場所がないとき) は、何も表示しない
	// (dock/ui/open_location.h の約束)。
	(void)ui::open_folder(QString::fromStdString(path_utf8));
}

// 画面共有の枠を埋めている Discord の人の数。自分の枠の人 (self_owner) は数えない (自分の枠を出している間は、その人の画面を
// Discord の枠に載せないため)。自分の枠を付けるかの判断 (DockPoll::step_self_frame) が使う。この数はスナップショットの
// 値なので、付けてよいかを最後に決めるのは Pipeline である (Pipeline::set_assign_policy。断られたら refuse_self_frame)。
int discord_slotted_of(const pipeline::PipelineSnapshot &snapshot, const std::string &self_owner)
{
	int count = 0;
	for (const pipeline::SlotSnapshot &row : snapshot.slots) {
		if (row.kind == config::SlotKind::Screen && !row.owner_user_id.empty() && row.owner_user_id != self_owner)
			++count;
	}
	return count;
}

} // namespace

DockPoll::DockPoll(pipeline::Pipeline &pipeline, platform::OsProbe &probe, DockWidget *widget, DockPollConfig cfg)
	: pipeline_(pipeline), probe_(probe), widget_(widget), cfg_(std::move(cfg))
{
	// 現在の設定は DockPoll が持つ。cfg_.settings は起動時に読んだ値のままにする。
	settings_ = cfg_.settings;
	// 起動時の値と同じ間は書かない。利用者が何も変えていないのに settings.json を書き直すと、cordmux が読めなかった
	// キーが消えるためである。
	settings_written_json_ = config::settings_to_json(settings_);
	// 自分の枠のチェックは、残してある値で始める。外したかと直前の共有は覚えておらず、共有の最中に起動したときは、
	// 最初のポーリングが共有の始まりとして、ほかの人と同じ規則で見る。
	self_frame_state_.on = settings_.self_frame_on;
	// 自分の枠を載せた順の印は、自分の枠が映った時に決める (update_self_added_mark)。割り当て表は起動のたびに空から始まるので、
	// 起動したときからチェックが付いていて、ほかの人より先に映れば、誰よりも先に載せた人 (印 0) になる。
	self_order_mark_ = sources::SelfOrderMark{};

	// 設定ファイルを読めなかった起動では、settings.json に一切書き込まない (write_settings)。そのことは、ここで
	// 1 回だけログに書く。書き込もうとするたびに書くと、設定を変えるたびや枠を手で動かすたびに、
	// 同じ行がリングバッファーを埋める。
	if (cfg_.settings_file_unreadable)
		diag::logf(LOG_INFO,
			   "Settings changes aren't saved this session because settings.json couldn't be read");

	// この起動で使っている設定。設定ウィンドウが、再起動が必要な項目を変えたかを判断するときに比べる。
	settings_state_.effective_at_start = cfg_.settings;
	// 起動時に読めなかったことを、メッセージに使うために OperationFacts に持つ。
	ops_.settings_unreadable_at_startup = cfg_.settings_file_unreadable;

	// 起動時の書き直し。Vesktop が起動していて書けなかったときは、Vesktop の終了を待ってもう一度試す
	// (dock_poll_install.cpp の on_probe_result)。
	rewrote_at_startup_ = cfg_.rewrote_vencord_dir_at_startup;
	startup_rewrite_waiting_ = static_cast<bool>(cfg_.retry_startup_rewrite);
}

DockPoll::~DockPoll()
{
	stop();
}

void DockPoll::start()
{
	if (timer_)
		return;

	look_save_ = std::make_unique<DeferredSave>(ledger::kLookSaveDelayMs, [this] { write_settings(); });
	if (!widget_.isNull()) {
		widget_->set_action_callback(
			[this](ledger::ActionId id, const ledger::ActionArgs &args) { run_action(id, args); });
		widget_->set_open_folder_callback(open_location_of);
		// 表示タブで値を変えたら、すぐ配信の画面に反映する。ソースの候補は、タブとコンボボックスを開いたときに OBS から集める。
		widget_->set_look_changed_callback([this](const config::Settings &tab) { on_look_changed(tab); });
		widget_->set_buffer_changed_callback([this](int ms) { on_buffer_changed(ms); });
		widget_->set_look_source_providers([] { return sources::list_self_frame_candidates(); },
						   [] { return sources::list_self_speaking_candidates(); });
		widget_->set_tab(settings_.dock_tab);
		// タブを替えたら設定に残し、1 行目と状態タブの件数をすぐに決め直す (状態タブを開くと既読になる)。押されたタブの
		// シグナルから戻った後に行う。
		widget_->set_on_tab_changed([this](config::DockTab tab) {
			settings_.dock_tab = tab;
			write_settings();
			if (timer_)
				QTimer::singleShot(0, timer_.get(), [this] { tick(); });
		});
		level_timer_ = std::make_unique<QTimer>();
		level_timer_->setInterval(kLevelPollMs);
		QObject::connect(level_timer_.get(), &QTimer::timeout, level_timer_.get(), [this] {
			if (widget_.isNull())
				return;
			const pipeline::AudioLevels levels = pipeline_.audio_levels();
			widget_->set_speaking(levels.speaking);
			widget_->set_audio_levels(levels.tracks);
		});
		widget_->set_on_levels_wanted([this](bool wanted) {
			if (!level_timer_)
				return;
			if (wanted)
				level_timer_->start();
			else
				level_timer_->stop();
		});
		if (widget_->levels_wanted())
			level_timer_->start();
	}

	// これまでに見た人の一覧を読む。最初の描画に使うので、ポーリングを始める前に読む。
	load_seen_people();
	ops_.seen_unreadable_at_startup = seen_file_unreadable_;
	// 一覧を読んだ直後に、載せる人の選び方と「自動で載せる」を付けた人を Pipeline に渡す (push_assign_policy)。
	// ここは obs_module_load の中で、シーンの読み込みが終わる前 (最初の configure を適用する前) なので、
	// 「自動で載せる」を付けた人を 1 人も知らないまま、最初の configure を適用することがない。
	push_assign_policy();
	// 覚えている参加者ごとの音量も同じ時点で渡す (最初の configure で組が名簿に入るとき、音量がもう表に入っている)。
	push_person_volumes();
	// 親を持たない QTimer を DockPoll が持つ。ウィジェットの子にすると、ウィジェットが OBS のメインウィンドウと
	// 一緒に破棄されたときに、DockPoll が持つポインターが無効になる (dock_bind.h の add_dock)。
	timer_ = std::make_unique<QTimer>();
	timer_->setInterval(cfg_.poll_ms > 0 ? cfg_.poll_ms : 500);
	QObject::connect(timer_.get(), &QTimer::timeout, timer_.get(), [this] { tick(); });
	timer_->start();
	// 枠の並べ方の周期は、ドックの表示の更新の間隔によらず固定にする。
	layout_timer_ = std::make_unique<QTimer>();
	layout_timer_->setInterval(layout_step_interval_ms(cfg_.poll_ms));
	QObject::connect(layout_timer_.get(), &QTimer::timeout, layout_timer_.get(), [this] { layout_step(); });
	layout_timer_->start();
	// 待ち受けは obs_module_load が開いたばかりなので、最初の世話は間隔が過ぎてからにする。
	last_maintain_listener_us_ = wall_now_us();

	step_and_tick(); // 最初の描画は待たずに行い、起動直後の画面を空にしない

	// Pipeline からの並べ直しの要求を受け付け、OBS を再起動しなくても反映される設定を渡す
	// (DockPollLayoutPart::attach)。
	DockPollLayoutPart::attach(*this);
}

void DockPoll::stop()
{
	// 表示タブで変えたまま渡していない値と、保存を待っている値を、今渡して保存する。ウィジェットのコールバックを外す前に行う。
	if (!widget_.isNull())
		widget_->flush_look();
	if (look_save_) {
		look_save_->flush();
		look_save_.reset();
	}
	// 枠と名前ラベルの処理のコールバックを、先に外す (DockPollLayoutPart::detach)。何度呼ばれてもよいように、
	// detach の側で確認している。
	DockPollLayoutPart::detach(*this);

	if (layout_timer_) {
		layout_timer_->stop();
		layout_timer_.reset();
	}
	if (timer_) {
		timer_->stop();
		timer_.reset();
	}
	// 音量の書き込みを待っているタイマーを止める。待っている分は、下の write_seen_people が書く。
	seen_flush_timer_.reset();
	// ウィジェットがまだあれば、押されても DockPoll を呼ばないようにする。この後で DockPoll が破棄されても、
	// 押されたボタンのコールバックが、破棄された DockPoll を呼ばない。
	if (!widget_.isNull()) {
		widget_->set_action_callback(nullptr);
		widget_->set_look_changed_callback(nullptr);
		widget_->set_buffer_changed_callback(nullptr);
		widget_->set_look_source_providers(nullptr, nullptr);
		widget_->set_on_tab_changed(nullptr);
		widget_->set_on_levels_wanted(nullptr);
	}
	level_timer_.reset();

	// ダミーを止める。sources は、OBS の終了前に set_dummy_mode(false) を必ず呼ぶことを求めている。
	// ダミーを表示していた時間帯の記録も、ここで閉じる。
	if (sources::dummy_mode_active())
		set_dummy_mode(false);

	// 今見えている人の最後に見た時刻を今にしてから書く (時刻は write_seen_people の中で更新する)。通話から
	// 抜けるのを待たずにここで確定させ、次の起動で、最後に見た時刻が古いままにならないようにする。
	write_seen_people();
	last_in_call_.clear();
}

void DockPoll::on_frontend_finished_loading()
{
	loading_finished_ = true;
	step_and_tick();
}

void DockPoll::on_streaming_event(ledger::StreamEvent event)
{
	// ポーリングを止めた後 (終了の処理の途中) は何もしない。アンインストールの後に押せるかは ledger::apply が決める。
	if (!timer_)
		return;
	const ledger::RecordFollow follow = ledger::record_follow_of(
		settings_.record_with_stream, event, pipeline_.snapshot().recording.enabled);
	if (follow == ledger::RecordFollow::None)
		return;
	diag::logf(LOG_INFO, "Streaming %s: %s the cordmux recording", event == ledger::StreamEvent::Started ? "started" : "stopped",
		   follow == ledger::RecordFollow::Start ? "starting" : "stopping");
	ledger::ActionArgs args;
	args.on = follow == ledger::RecordFollow::Start;
	run_action(ledger::ActionId::ToggleRecording, args);
}

void DockPoll::on_theme_changed()
{
	// 開いている cordmux のウィンドウにも伝える。ドックのウィジェットは自分の StyleChange でも読み直すが、
	// OBS のイベントを先に受けた時点で、1 回そろえておく。
	if (!widget_.isNull())
		widget_->on_theme_changed();
	DockPollSettingsPart::on_theme_changed(*this);
}

DockFacts DockPoll::current_facts(bool frozen) const
{
	DockFacts facts;
	facts.now_us = wall_now_us();
	facts.utc_offset_sec = QDateTime::currentDateTime().offsetFromUtc();
	facts.video_stall_threshold_us = cfg_.video_stall_threshold_us;

	// cordmux の有効と無効は、settings の feature_enabled から取る (利用者が選んだ値を次の起動にも残す)。
	// 録画のオンとオフは、ここでは渡さない。それを持つのは Pipeline のメモリだけで、build_observation が
	// snapshot から直接読む (build_observation.h の DockFacts)。
	facts.feature = settings_.feature_enabled ? ledger::FeatureSwitch::On : ledger::FeatureSwitch::Off;
	// settings からコピーする、録画と画質の上限の 4 つの値。
	facts.record_dir = settings_.record_dir;
	facts.record_output_pcm = settings_.record_output_pcm == config::RecordPcmFormat::Int24
					  ? ledger::RecordPcm::Int24
					  : ledger::RecordPcm::Float32;
	facts.limit_max_long_edge = settings_.limit_max_long_edge;
	facts.limit_fps_divisor = settings_.limit_fps_divisor;
	// ダミーは sources のフラグから取る。ダミーの状態は、フレームを渡す関数が読む 1 つの atomic だけが持ち、
	// ドックはそれを読むだけである。
	facts.dummy = sources::dummy_mode_active() ? ledger::FeatureSwitch::On : ledger::FeatureSwitch::Off;
	facts.broadcaster_user_id = settings_.broadcaster_user_id;
	facts.slot_order = settings_.slot_order;
	facts.self_slot_first = settings_.self_slot_first;
	facts.self_frame_added_mark = self_order_mark_.mark;
	// 全画面。全画面にできる人と、全画面の人の配信に出している名前は、合成のソースに最後に渡した配置の入力と、配置に描いて
	// いない画面共有している人から取る。
	facts.focus_user_id = focus_user_id_;
	for (const sources::CompositeFocusTile &tile : layout_state_.focus_tiles) {
		facts.focus_candidates.push_back(tile.user_id);
		if (!focus_user_id_.empty() && tile.user_id == focus_user_id_ && facts.focus_name.empty())
			facts.focus_name = tile.name;
	}
	// 配置に描いていないが、画面共有している人も全画面にできる (枠に載せていない人)。
	for (const sources::CompositeFocusOther &other : layout_state_.focus_others) {
		facts.focus_candidates.push_back(other.user_id);
		if (!focus_user_id_.empty() && other.user_id == focus_user_id_ && facts.focus_name.empty())
			facts.focus_name = other.name;
	}
	facts.last_known_people = last_known_people_;
	facts.last_known_vesktop_user_id = last_known_vesktop_user_id_;
	facts.last_known_vesktop_name = last_known_vesktop_name_;
	facts.roster_show = settings_.roster_show;
	facts.roster_show_bots = settings_.roster_show_bots;
	facts.assign_mode = settings_.assign_mode == config::AssignMode::Auto ? ledger::AssignMode::Auto
									      : ledger::AssignMode::Manual;
	facts.receive_scope = settings_.receive_scope == config::ReceiveScope::Slots ? ledger::ReceiveScope::Slots
										      : ledger::ReceiveScope::Everyone;
	facts.composite_present = sources::composite_present();

	// これまでに見た人の一覧。並べ方も絞り込みも ledger::decide_dock が決めるので、持っているものをそのまま渡す。
	facts.seen.people = seen_people_;
	facts.seen.discarded_with_marks = seen_discarded_with_marks_;
	facts.seen.call_joined_at_us = call_joined_at_us_;

	// 合成のソースの有無を調べ終えたとするのは、シーンの読み込みが終わってからにする (dock_poll.h の
	// on_frontend_finished_loading)。シーンコレクションを切り替えている途中 (frozen) も調べ終えたとしない。
	// 利用者が自分で切り替えている途中の状態を、cordmux をまだ置いていないと表示しないためである。
	if (loading_finished_ && !frozen) {
		facts.composite_checked = true;

		// 声が二重に配信に入る構成か。読むのは上と同じ条件のときだけにする。読み込みの前と切り替えの途中は、
		// アプリケーション音声キャプチャのソースも cordmux のソースもそろっていないので、ないとは断定しない。
		const sources::DoubleAudioState audio = sources::read_double_audio_state();
		facts.discord_audio_source = audio.discord_capture_name.has_value() ? ledger::DiscordAudioSource::Found
										  : ledger::DiscordAudioSource::NotFound;
		facts.discord_audio_source_name = audio.discord_capture_name.value_or(std::string());
		facts.composite_active = audio.composite_active;
		facts.composite_audible = audio.composite_audible;
	}

	facts.plugin_version = cfg_.plugin_version;
	// 同梱の Vencord のバージョンは、data/vencord-src の cordmux-build.txt から取る。読めなければ空のままにし、
	// 画面には "-" を表示して、バージョンが違うというメッセージは表示しない。Vesktop のバージョンは、ここでは渡さない。
	// 送信側が hello で伝えるので、build_observation が snapshot の listener からコピーする
	// (docs/wire_format.md の「メッセージの種類」)。
	if (original_mark_.has_value()) {
		facts.bundled_vencord_version = original_mark_->build_id;
		facts.bundled_vencord_commit = original_mark_->vencord_commit;
		facts.bundled_vencord_tag = original_mark_->vencord_tag;
	}

	// インストールに関わる、DockPoll が持つ情報。
	facts.rewrote_at_startup = rewrote_at_startup_;
	facts.reinstall_failed_files = reinstall_failed_files_;
	// この起動では Vesktop の設定を書き込まないか。cfg_ の値は起動時に決まった実際の値で、settings.json を
	// 読めなかった起動では、plugin-main.cpp が false にしてから渡す (pipeline::effective_allow_state_json_write)。
	// [Vesktopに追加] が書き込まずに WritesDisabled で終わる条件 (dock_poll_install.cpp の write_vencord_dir) と
	// 同じ値を見るので、ボタンを表示しない判断と、押したときの結果が食い違わない。
	// 2 つの原因 (設定で無効にしたか、settings.json を読めなかったか) は、
	// OperationFacts::settings_unreadable_at_startup で区別する。
	facts.vesktop_writes_off = !cfg_.allow_state_json_write;

	// アンインストールの状態。配信中と録画中は OBS の状態で、アンインストールできない理由を表示するために、
	// UI スレッドのここでだけ読む。
	facts.uninstall = uninstall_facts_of(uninstall_phase_, uninstall_report_);
	facts.uninstall.obs_streaming = obs_frontend_streaming_active();
	facts.uninstall.obs_recording = obs_frontend_recording_active();
	// 仮想カメラとリプレイバッファの動作中も、アンインストールできないようにする。アンインストールは
	// シーンコレクションを順に切り替えてソースを消すので、その途中の画面が、仮想カメラの映像 (会議や Discord) と
	// リプレイバッファの保存に入る。配信と録画と同じ所で読み、4 つの値を同じ時点のものにそろえる。
	facts.uninstall.obs_virtual_camera = obs_frontend_virtualcam_active();
	facts.uninstall.obs_replay_buffer = obs_frontend_replay_buffer_active();

	// 設定ウィンドウに表示する OBS と OS のバージョンと GPU の名前。読むのは 1 回だけ (read_platform_versions)。
	facts.obs_version = obs_version_;
	facts.os_version = os_version_;
	facts.gpu_name = gpu_name_;
	return facts;
}

void DockPoll::read_platform_versions()
{
	if (!platform_versions_read_) {
		platform_versions_read_ = true;
		obs_version_ = obs_version_text();
		// OS のバージョンは platform/ の関数で読む。Windows に依存する OBS の API (util/windows) は、
		// platform/ の中でだけ使う。返す形は「10.0.<ビルド>.<リビジョン>」である。
		os_version_ = platform::os_version_text();
	}
	// GPU の名前は、シーンの読み込みが終わってから読む。描画のコンテキストを取るので、起動の途中で描画スレッドを
	// 待たせないためである。
	if (!gpu_name_read_ && loading_finished_) {
		gpu_name_read_ = true;
		gpu_name_ = gpu_name_text();
	}
}

void DockPoll::step_and_tick()
{
	layout_step();
	tick();
}

void DockPoll::layout_step()
{
	const pipeline::PipelineSnapshot snapshot = pipeline_.snapshot();

	// 自分の枠に映すソースを探し直す。自分の枠が表示されているかを調べる前に済ませて、同じ判断に反映する。
	DockPollLayoutPart::sync_self_frame_source(*this, snapshot);

	// 自分の枠を出すかの状態を進める。映すソースが結び付いているかは、シーンの読み込みが終わってからだけ見る。読み込みの前に
	// 見ても、結び付いていないとしか判断できないためである。結び付いていない自分の枠は配置に数えない。数えると、4 人でも
	// 5 人分の配置になる。
	const bool bound_known = loading_finished_ && !snapshot.frozen;
	const bool bound = bound_known && sources::self_frame_shown();
	step_self_frame(snapshot, bound, &self_frame_facts_);
	// 自分の行のチェックが変わったら (共有の始まりで付いた、など)、画面の枠に載せられる人数を Pipeline に渡し直す。付ける判断は
	// スナップショットの人数で行ったので、その後に自動で 4 人目が載っていれば Pipeline が断り、チェックを外して、下の配置の
	// 入力からも自分の枠を外す (refuse_self_frame)。
	push_self_frame_in_use_if_changed();

	// 合成のソースへの配置の入力を渡す。
	push_layout_now(snapshot, self_frame_facts_.self_frame_shown);
}

void DockPoll::tick()
{
	// スナップショットを取る前に、録画が自分で止まったか (空き容量がなくなったとき) と、録画を始められなかったかを
	// 確認する。スナップショットの録画のオンとオフは Pipeline のフラグから取る (pipeline.cpp の snapshot) ので、
	// ここでフラグを false にしておかないと、録画が止まったのに、ボタンが [cordmuxの録画を終了] のまま、
	// 1 回のポーリングの間残る。
	pipeline_.poll_recording_self_stop();

	// 待ち受けの世話は、スナップショットを取る前に済ませる。開き直せたときに、同じポーリングの画面から別の OBS の
	// メッセージが消える。
	const std::int64_t now_us = wall_now_us();
	if (cfg_.maintain_listener && maintain_listener_due(now_us, last_maintain_listener_us_)) {
		last_maintain_listener_us_ = now_us;
		cfg_.maintain_listener();
	}

	// 参加者ごとの届くまでの遅れ (直近 1 分の列) は、状態タブが見えている間だけコピーする。Pipeline は列を常に記録しているので、
	// タブを開いた瞬間から直近 1 分の全部が出る。映像途切れの累計は、タブが見えていなくても下で数える。
	pipeline_.set_arrival_watch(!widget_.isNull() && widget_->status_tab_open());
	const pipeline::PipelineSnapshot snapshot = pipeline_.snapshot();
	const platform::OsProbeResult probe = probe_.latest();

	// 設定ウィンドウに表示する、OBS と OS のバージョンと GPU の名前。1 回読めば、後はコピーするだけ。
	read_platform_versions();

	// OsProbe の結果が新しくなっていれば、cordmux-build.txt を読み直し、data/vencord に前のバージョンの
	// ファイルがあれば、メッセージを表示せずにコピーし直す。画面を作る前に済ませる。
	on_probe_result(probe);

	// 通話に参加した人と抜けた人を、一覧に反映する。画面を作る前に済ませて、同じポーリングの一覧に
	// 表示されるようにする。
	update_seen_people(snapshot, now_us);

	// シーンコレクションの切り替えの途中は、ソースが一時的になくなるので、調べずに未確認のまま渡す (current_facts)。
	update_self_added_mark(snapshot);
	DockFacts facts = current_facts(snapshot.frozen);
	facts.now_us = now_us;
	// 前のバージョンのファイルをコピーし直した直後は、その前の照合の結果で、上書きされていると表示しない。
	// 次の照合の結果が届けば、普通の判断に戻る。
	facts.hide_hash_mismatch =
		stale_copied_for_probe_at_.has_value() && *stale_copied_for_probe_at_ == probe.probed_at_us;
	// Vesktop の設定を書く直前に見た Vesktop の状態 (vesktop_free_to_write)。見た後に OsProbe の結果が届いたら、そちらを
	// 使う (見た後の問い合わせなので、同じか新しい)。
	if (vesktop_process_seen_.has_value() && probe.probed && probe.probed_at_us > vesktop_process_seen_at_us_)
		vesktop_process_seen_.reset();
	// 接続していない間に Vesktop が終了したかを、OsProbe の間隔 (30 秒) を待たずに調べる (dock_poll_config.h の
	// vesktop_quit_check_due)。[Vesktopに追加] と [Vesktopから削除] は、Vesktop を終了した直後から押せる必要がある。
	// 終了を見たら、OsProbe の新しい結果が届くまで「起動していない」として画面を作り、OsProbe にすぐ問い合わせ直させる
	// (起動時の書き直しを待っているときは、その結果で書き直す。dock_poll_install.cpp の on_probe_result)。
	const bool vesktop_seen_running = vesktop_process_seen_.has_value()
						  ? *vesktop_process_seen_ != ledger::VesktopProcess::NotRunning
						  : probe.probed && probe.vesktop_process;
	// Vesktop の終了で押せるようになる操作があるか (前の周期の Observation で見る)。cordmux を無効にしている間と、Vesktop の
	// 設定を読めていない間は、[Vesktopに追加] のカードも [Vesktopから削除] の組も出ないので、毎秒は調べない。
	const bool waits_for_quit =
		(settings_.feature_enabled && last_observation_.install.dir != ledger::VencordDirState::Unknown) ||
		startup_rewrite_waiting_;
	if (vesktop_quit_check_due(vesktop_seen_running, was_link_connected_, writes_stopped(), waits_for_quit, now_us,
				   last_vesktop_quit_check_us_)) {
		last_vesktop_quit_check_us_ = now_us;
		if (platform::vesktop_running_now() == std::optional<bool>(false)) {
			vesktop_process_seen_ = ledger::VesktopProcess::NotRunning;
			vesktop_process_seen_at_us_ = now_us;
			probe_.request_probe_now();
		}
	}
	facts.vesktop_process_seen = vesktop_process_seen_;

	// 自分の枠の判断の結果は layout_step が出したものを使う (状態は tick では進めない)。配置の入力も layout_step が渡す。
	// 映す映像を OBS から選んでいるかは設定の値なので、設定を替えたらすぐ反映する (layout_step を待たない)。
	facts.self_frame_selected = self_frame_selected();
	facts.self_frame_bound = self_frame_facts_.self_frame_bound;
	facts.self_frame_on = self_frame_facts_.self_frame_on;
	facts.self_frame_shown = self_frame_facts_.self_frame_shown;
	facts.self_frame_share = self_frame_facts_.self_frame_share;

	last_observation_ = build_observation(snapshot, probe, facts);
	// 接続している間の参加者を覚えておく。接続が切れても、参加者タブの行を消さずに残すためである。
	if (last_observation_.link.state == ledger::LinkState::Connected)
		last_known_people_ = last_observation_.people;
	// Vesktop のアカウントも、分かったときに覚える。接続が切れた後も、サブの名前と [変更] の候補を変えないためである。
	if (!last_observation_.vesktop_user_id.empty()) {
		if (last_known_vesktop_user_id_ != last_observation_.vesktop_user_id)
			last_known_vesktop_name_.clear();
		last_known_vesktop_user_id_ = last_observation_.vesktop_user_id;
		if (!last_observation_.vesktop_name.empty())
			last_known_vesktop_name_ = last_observation_.vesktop_name;
	}
	// DockPoll が持つ値をコピーする。操作の結果、最初のフレームを待つ時間、
	// 残り時間を警告するしきい値 (設定を変えたらすぐに反映される)。
	// 押した [移動] と [退出] は、結果が出たか、接続が切れたら忘れる (忘れないと、後で通話が変わったときに失敗と誤る)。
	ops_.call_action_timeout_us = static_cast<std::int64_t>(settings_.call_action_timeout_ms) * 1000;
	last_observation_.ops = ops_;
	if (ledger::call_action_finished(last_observation_)) {
		ops_.call_action.reset();
		last_observation_.ops.call_action.reset();
	}
	last_observation_.first_frame_wait_threshold_us =
		static_cast<std::int64_t>(settings_.first_frame_wait_ms) * 1000;
	// 映像を受け取ると決めていない状態から戻った時刻。待ち時間はこの時刻から数える。
	const bool video_expected = ledger::video_expected(last_observation_);
	if (video_expected && !was_video_expected_)
		video_expected_since_us_ = now_us;
	was_video_expected_ = video_expected;
	last_observation_.video_expected_since_us = video_expected_since_us_;
	last_observation_.recording.space_warn_seconds =
		static_cast<std::int64_t>(settings_.record_space_warn_minutes) * 60;

	// Discord の内部への依存箇所ごとに、最後に読めた時刻を記録する (診断情報のエクスポートで使う)。
	update_contact_success(&contact_last_ok_, last_observation_.contacts, snapshot.facts.received, now_us);

	// OsProbe の問い合わせの間隔は、Vesktop と接続しているかどうかで変わる。
	const bool link_connected = last_observation_.link.state == ledger::LinkState::Connected;
	probe_.set_link_connected(link_connected);
	// 接続が切れたときは、間隔を待たずに 1 回問い合わせる (link_just_lost)。
	if (link_just_lost(was_link_connected_, link_connected))
		probe_.request_probe_now();
	was_link_connected_ = link_connected;

	// 表示の規則 (純粋関数) で判断し、表示するまでの待ちと表示した順 (状態は DockPoll が持つ) を適用してから描く。
	const ledger::dockview::DockViewV2 raw = ledger::decide_dock(last_observation_);
	ledger::NoticeTrackerConfig tracker_cfg;
	tracker_cfg.show_delay_us = static_cast<std::int64_t>(settings_.notice_show_delay_ms) * 1000;
	tracker_cfg.clear_delay_us = static_cast<std::int64_t>(settings_.notice_clear_delay_ms) * 1000;
	ledger::StabilizeResult stable = ledger::stabilize(tracker_state_, raw, now_us, tracker_cfg);
	tracker_state_ = std::move(stable.next);
	// 1 行目の中身を決める。既読にするのは、状態タブを開いていて、ドックが見えている間だけである。
	ledger::FirstLineInput line_input;
	line_input.now_us = now_us;
	line_input.streaming = facts.uninstall.obs_streaming.value_or(false);
	line_input.status_tab_open = !widget_.isNull() && widget_->status_tab_open();
	ledger::FirstLineResult settled = ledger::settle_first_line(read_state_, stable.view, line_input);
	read_state_ = std::move(settled.next);
	stable.view.first_line = std::move(settled.first_line);
	// 状態タブ。累計と履歴は、タブが見えていなくても進める。
	ledger::StatusInput status_input;
	status_input.now_us = now_us;
	status_input.utc_offset_sec = facts.utc_offset_sec;
	status_input.streaming = line_input.streaming;
	ledger::StatusResult status = ledger::settle_status(status_state_, stable.view, last_observation_, status_input);
	status_state_ = std::move(status.next);
	stable.view.status_tab = std::move(status.view);
	if (!widget_.isNull()) {
		widget_->set_confirm_inputs(line_input.streaming, settings_.confirm_while_streaming);
		widget_->set_look_values(settings_);
		obs_video_info ovi = {};
		if (obs_get_video_info(&ovi))
			widget_->set_canvas_size(static_cast<int>(ovi.base_width), static_cast<int>(ovi.base_height));
		widget_->set_layout_hold_idle_ms(settings_.layout_hold_idle_ms);
		widget_->set_status_tab_label(settled.status_tab_label);
		// 名前ラベルに渡している画像と同じものを、行のアバターにする。
		std::map<std::string, PeopleList::AvatarBytes> avatars;
		for (const auto &[user_id, image] : snapshot.profiles.avatars)
			avatars.emplace(user_id, image.png);
		widget_->set_avatars(avatars);
		widget_->render(stable.view);
		// 押した操作の処理中の表示。フラグは dock_poll_install.cpp が立てる (DockPollInstallState)。
		std::set<ledger::ActionId> busy;
		if (install_state_.adding_to_vesktop) {
			busy.insert(ledger::ActionId::WriteVencordDir);
			busy.insert(ledger::ActionId::RepairVencordDir);
		}
		widget_->set_busy_actions(busy);
	}
	report_notice_changes(stable.diff, stable.view.first_line);
	rendered_people_ = !stable.view.people.empty();
	// 開いているウィンドウ (設定ウィンドウの「バージョン情報」と、参加者の設定ウィンドウ) を、同じ View で
	// 書き換える。
	DockPollSettingsPart::refresh_windows(*this, stable.view);

	// 表示名を、名前ラベルに渡す。名前は、参加者の一覧と同じ effective_display_name で決める。
	DockPollLayoutPart::distribute_person_decor(*this, snapshot);

	// 同じ名前を録画にも渡す。セグメントを開いた時点で録画ログに書く名前は、この effective_display_name が決めた
	// 名前である。録画ログとファイル名は実際の人の名前なので、ダミーの間も渡す。変わっていなければ何もしない処理は、
	// Pipeline::set_display_names の側にある。
	std::map<std::string, std::string> record_names;
	for (const auto &[stream_id, format] : snapshot.stream_formats) {
		(void)stream_id;
		if (format.user_id.empty() || record_names.count(format.user_id) != 0)
			continue;
		std::string name = effective_display_name(seen_people_, snapshot.profiles, format.user_id);
		if (name.empty())
			continue; // 名前を取得できていない人は入れない (録画の側が userId をそのまま使う)
		record_names.emplace(format.user_id, std::move(name));
	}
	pipeline_.set_display_names(std::move(record_names));
}

void DockPoll::report_notice_changes(const ledger::NoticeDiff &diff, const ledger::dockview::FirstLine &first_line)
{
	// OBS のログには、メッセージが表示されるか消えるたびに 1 行書く。書くのは規則の名前、メッセージのレベル、対象
	// (枠の番号) だけで、メッセージのキー、userId、名前は書かない。表示した行のログレベルはメッセージのレベルに合わせる
	// (メッセージのレベル 1 は LOG_ERROR、メッセージのレベル 2 は LOG_WARNING、ほかは LOG_INFO)。消えた行は LOG_INFO に
	// する (直ったことは問題ではないため)。
	// 行の形は test/tools/notice-count/notice_count.py が読む。
	for (const ledger::NoticeEvent &e : diff.appeared) {
		diag::logf(log_level_of(e.log_level), "Notice shown (rule=%s tier=%s subject=%s)", e.rule_name.c_str(),
			   tier_log_name(e), e.log_subject.empty() ? "-" : e.log_subject.c_str());
	}
	for (const ledger::NoticeEvent &e : diff.disappeared) {
		diag::logf(LOG_INFO, "Notice cleared (rule=%s tier=%s subject=%s)", e.rule_name.c_str(), tier_log_name(e),
			   e.log_subject.empty() ? "-" : e.log_subject.c_str());
	}

	// スクリーンリーダーの読み上げ。何を読み上げるかは dock_widget.h の announcements_for が決める (ドックを
	// 操作するテストも同じ関数を使う)。メッセージのレベル 1 が消えたときは、消えた後の 1 行目のテキストを渡す。
	// 枠がすべて使用中になったか空いたかは、一覧が前から表示されていたときだけ渡す (dock_poll.h の
	// rendered_people_)。フォーカスは動かさない。
	if (widget_.isNull())
		return;
	for (const Announcement &a : announcements_for(diff, first_line, rendered_people_))
		widget_->announce(a.text, a.assertive);
}

void DockPoll::run_action(ledger::ActionId id, const ledger::ActionArgs &args)
{
	// 押せるかどうかを決めるのは ledger::apply() だけである。
	const ledger::Intent intent = ledger::apply(last_observation_, id, args);
	if (!intent.can_run()) {
		diag::logf(LOG_INFO, "Ignored %s: it isn't available right now", action_name(id));
		return;
	}

	switch (intent.kind) {
	case ledger::IntentKind::WriteVencordDir: {
		// WriteVencordDir と RepairVencordDir (どちらも [Vesktopに追加]) は同じ処理で、押された状況だけが違う。
		// 状況は押された操作の id で分ける (IntentKind はどちらも WriteVencordDir)。
		const install::WriteScene scene = id == ledger::ActionId::RepairVencordDir
							  ? install::WriteScene::FixRequested
							  : install::WriteScene::Pressed;
		run_dialog_action([this, scene]() { write_vencord_dir(scene); });
		break;
	}
	case ledger::IntentKind::ReinstallVencordFiles:
		// data/vencord-src からコピーし直し、バックアップの written_at を進めて、OsProbe に問い合わせ直させる。
		// ダイアログを開かないので、押されたボタンのシグナルの中でそのまま実行する。
		reinstall_vencord_files(true);
		break;
	case ledger::IntentKind::RemoveFromVesktop:
		// Vesktop の設定を cordmux を追加する前の値に戻す。確認のダイアログを開かない ([Vesktopに追加] で戻せる) ので、
		// [再インストール] と同じく、押されたボタンのシグナルの中でそのまま実行する。読み書きするのは小さな 2 つの
		// 設定のファイルとバックアップだけである。
		remove_from_vesktop();
		break;
	case ledger::IntentKind::SetFeature:
		set_feature(intent.args.on);
		break;
	case ledger::IntentKind::Uninstall:
		// 確認のダイアログを開いてから、アンインストールする。
		run_dialog_action([this]() { uninstall_now(); });
		break;
	case ledger::IntentKind::OpenSettings:
		DockPollSettingsPart::open_settings(*this, intent.args.page, /*start_export=*/false);
		break;
	case ledger::IntentKind::OpenParticipants:
		DockPollSettingsPart::open_participants(*this);
		break;
	case ledger::IntentKind::SetDummy:
		set_dummy_mode(intent.args.on);
		break;
	case ledger::IntentKind::SetSlot:
		set_slot(intent.args);
		break;
	case ledger::IntentKind::ReplaceSlot:
		replace_slot(intent.args);
		break;
	case ledger::IntentKind::ResetStallTally:
		status_state_ = ledger::reset_status(status_state_);
		break;
	case ledger::IntentKind::SetFocus:
		set_focus(intent.args.user_id);
		break;
	case ledger::IntentKind::SetMainAccount:
		set_main_account(intent.args.user_id);
		break;
	case ledger::IntentKind::SetAlwaysOn:
		// 「自動で載せる」の設定。一覧のファイルにだけ書き、割り当て表は変えない。誰をどの枠に載せるかは、
		// SetSlot の処理である。
		if (!intent.args.user_id.empty()) {
			config::ensure_seen_person(seen_people_, intent.args.user_id).always_on = intent.args.on;
			write_seen_people();
			// 設定した人がもう画面共有をしていれば、次のポーリングを待たずに、すぐ枠に載る。
			push_assign_policy();
		}
		break;
	case ledger::IntentKind::SetNameOverride:
		// 名前の変更。空の名前は、変えた名前をやめて元に戻すことを表す (ledger::ActionArgs::name)。
		if (!intent.args.user_id.empty()) {
			ledger::SeenPerson &person = config::ensure_seen_person(seen_people_, intent.args.user_id);
			if (intent.args.name.empty())
				person.name_override.reset();
			else
				person.name_override = intent.args.name;
			write_seen_people();
			// 名前ラベルにも、次のポーリングを待たずに、ここで渡し直す。
			DockPollLayoutPart::distribute_person_decor(*this, pipeline_.snapshot());
		}
		break;
	case ledger::IntentKind::SetRosterShown:
		// 画面共有していない参加者の並びに出すか (参加者の設定ウィンドウの列「表示」)。一覧のファイルにだけ書く。合成の
		// ソースの並びには、次のポーリングで渡る (dock_poll_layout.cpp の push_composite_input)。
		if (!intent.args.user_id.empty()) {
			config::ensure_seen_person(seen_people_, intent.args.user_id).roster_hidden = !intent.args.on;
			write_seen_people();
		}
		break;
	case ledger::IntentKind::SetVoiceMute:
		set_person_mute(intent.args, pipeline::MixerKind::Voice);
		break;
	case ledger::IntentKind::SetScreenAudioMute:
		set_person_mute(intent.args, pipeline::MixerKind::ScreenAudio);
		break;
	case ledger::IntentKind::SetVoiceVolume:
		set_person_volume(intent.args, pipeline::MixerKind::Voice);
		break;
	case ledger::IntentKind::SetScreenAudioVolume:
		set_person_volume(intent.args, pipeline::MixerKind::ScreenAudio);
		break;
	case ledger::IntentKind::SetVoiceSource:
		// 「使う声」の設定。一覧のファイルにだけ書く (元の値は seen_people.json にあり、割り当て表は
		// Pipeline がそのコピーから作り直す)。空か本人の userId は、本人の声に戻すことを表す
		// (ledger::ActionArgs::voice_user_id)。書いたらすぐに Pipeline に渡し、設定した時点で
		// 声が置き換わるようにする (「自動で載せる」と同じく push_assign_policy を使う)。ログには設定したか
		// 戻したかだけを書き、userId は書かない。ログに人の識別子を残さないためである (pipeline.cpp の facts の
		// ログと同じ)。
		if (!intent.args.user_id.empty()) {
			ledger::SeenPerson &person = config::ensure_seen_person(seen_people_, intent.args.user_id);
			const bool reset =
				intent.args.voice_user_id.empty() || intent.args.voice_user_id == intent.args.user_id;
			if (reset)
				person.voice_source_user_id.reset();
			else
				person.voice_source_user_id = intent.args.voice_user_id;
			write_seen_people();
			push_assign_policy();
			diag::logf(LOG_INFO, "%s",
			           reset ? "Voice From set back to Own Voice"
			                 : "Voice From set to another person's voice");
		}
		break;
	case ledger::IntentKind::SetSelfFrame:
		set_self_frame(intent.args.on);
		break;
	case ledger::IntentKind::MoveToMainCall:
		// 押したときだけ送信側に伝える。移動先は伝えず、送信側が押された時点のメインアカウントの通話を読み直す。
		// 押せる条件 (接続している、メインアカウントが別の通話に参加している、など) は ledger::apply が先に見ている。
		// 送信側は失敗しても何も返さないことがあるので、押した時刻を覚えて、通話が変わらなければメッセージにする
		// (ledger::call_action_failed)。
		ops_.call_action = ledger::CallActionPress{ledger::CallActionKind::Move, wall_now_us()};
		pipeline_.request_move_to_main_call();
		break;
	case ledger::IntentKind::LeaveCall:
		ops_.call_action = ledger::CallActionPress{ledger::CallActionKind::Leave, wall_now_us()};
		pipeline_.request_leave_call();
		break;
	case ledger::IntentKind::SetRecording:
		set_recording(intent.args.on);
		break;
	case ledger::IntentKind::ResumeFinalize:
		// 途中で止まった録画ファイルの作成を、続きから実行する。
		pipeline_.resume_finalize();
		break;
	case ledger::IntentKind::ExportBundle:
		// どこから押しても、設定ウィンドウを「バージョン情報」のページで開いてから、同じ処理を始める。
		// 結果をそのページのボタンの下に表示するためである。
		DockPollSettingsPart::open_settings(*this, ledger::SettingsPage::About, /*start_export=*/true);
		break;
	default:
		// 実行する処理のない操作。何もしないまま実行したように見せないように、ログに書く。
		diag::logf(LOG_WARNING, "Ignored %s: it does nothing in this version", action_name(id));
		break;
	}

	// 押した結果を、次のポーリングを待たずに画面に反映する。ここで新しくなるのは受信側の情報
	// (Pipeline::snapshot) で、OS の情報は OsProbe のワーカーが持っている。そのため OS の状態を変える操作は、
	// その処理の中で OsProbe::request_probe_now() を呼ぶ (write_vencord_dir の最後)。その結果は、問い合わせが
	// 終わった後のポーリングで画面に表示される。
	// ただし、イベントループに 1 度戻ってから読み直す。今はまだ押されたボタンのシグナルの途中で、ここで読み直すと、
	// そのボタンが載っているカードごと作り直されるためである。notice_list.cpp も deleteLater でこの問題を
	// 避けているが (retire_widget)、描き直す側の都合を、描かれる側の実装に頼らない。戻ってから読み直しても、
	// 利用者には、すぐに反映されたように見える。
	if (timer_)
		QTimer::singleShot(0, timer_.get(), [this] { step_and_tick(); });
	else
		step_and_tick();
}

void DockPoll::run_dialog_action(std::function<void()> work)
{
	if (dialog_action_running_) {
		diag::logf(LOG_INFO, "Ignored a press while a dialog was open");
		return;
	}
	dialog_action_running_ = true;
	auto body = [this, work = std::move(work)]() {
		work();
		dialog_action_running_ = false;
	};
	// 押されたボタンのシグナルから戻った後に実行する (dock_poll.h の run_dialog_action)。ポーリングを止めた後
	// (timer_ がないとき) はダイアログを開く場面ではないので、そのまま実行する。
	if (timer_)
		QTimer::singleShot(0, timer_.get(), body);
	else
		body();
}

bool DockPoll::writes_stopped() const
{
	return uninstall_phase_ != ledger::UninstallPhase::NotStarted || install::is_uninstalling();
}

void DockPoll::set_feature(bool on)
{
	// 選んだ値を設定に保存し、次の起動にも残す。無効にしても接続の待ち受けは続ける。送信側に media を false にして
	// 送るのは、notify_runtime_settings から呼ばれる Pipeline::set_feature_enabled である。
	if (settings_.feature_enabled == on)
		return;
	settings_.feature_enabled = on;
	write_settings();
	// 録画中なら録画を終了し、録画ファイルの作成を始める。録画のボタンと同じ set_recording を使う (スナップショットと同じ値なら
	// 何もしない)。全画面もやめる。
	if (!on) {
		set_recording(false);
		set_focus(std::string());
	}
	diag::logf(LOG_INFO, "%s cordmux", on ? "Enabled" : "Disabled");
	notify_runtime_settings();
}

void DockPoll::notify_runtime_settings()
{
	if (cfg_.apply_runtime_settings)
		cfg_.apply_runtime_settings(settings_);
}

void DockPoll::set_recording(bool on)
{
	// 変わっていなければ呼ばない。Pipeline の側でも確認しているが、ここは押されたボタンと周期ごとの
	// スナップショットが競合する所なので、ドックでも確認する。画面はスナップショットの値でボタンのテキストを
	// 決めているので、スナップショットが古いうちに 2 回押されると、同じ値が続けて届く。比べるのは現在の
	// スナップショットだけである。ドックは録画のオンとオフを持たず、持つのは Pipeline のメモリだけである。
	if (pipeline_.snapshot().recording.enabled == on)
		return;
	pipeline_.set_recording(on);
}

void DockPoll::set_dummy_mode(bool on)
{
	sources::set_dummy_mode(on);
	// ダミーを表示していた時間帯の一覧と、そのログの行は、Pipeline が書く (Pipeline::note_dummy_mode)。
	pipeline_.note_dummy_mode(on);
}

void DockPoll::push_assign_policy()
{
	// 「自動で載せる」を付けた人の集合を、一覧から作る (元の値は seen_people.json にある)。
	std::set<std::string> always_on;
	for (const ledger::SeenPerson &person : seen_people_) {
		if (person.always_on && !person.user_id.empty())
			always_on.insert(person.user_id);
	}
	// 「使う声」の組み合わせも、同じ一覧から作る。ファイルに書かれた組み合わせをそのまま渡し、1 対 1 に絞るのは
	// Pipeline の側である (pipeline/stream_router.h が config::usable_voice_links を使う)。ここで絞った結果を
	// 渡すと、絞り方を 2 か所に持つことになる。ここでは、絞られて使われない組み合わせの数だけをログに書く
	// (一覧のファイルを手で書き換えたときの、重なりと連鎖)。
	std::map<std::string, std::string> voice_links = ledger::voice_source_marks(seen_people_);
	const std::size_t usable = config::usable_voice_links(voice_links).size();
	if (usable < voice_links.size())
		diag::logf(LOG_WARNING,
		           "Ignored %zu Voice From settings: a voice is used for 2 people or chained",
		           voice_links.size() - usable);
	// メインアカウントの userId も渡す。自分の声の扱い (合成のソースがあるときだけ声の枠に入れ、ミキサーでは既定で
	// ミュートにする) が、この人の声に適用される (pipeline/stream_router.h の AssignPolicy、pipeline/mixer_roster.h の
	// MixerLevels)。自分の枠を使っているかも渡す (自動で載せる経路が、手動の操作と同じ人数までしか載せない)。使い始めることは
	// Pipeline が割り当て表と同じロックの中で受け付けるか断り、断られたら自分の行のチェックを外す (refuse_self_frame)。
	const bool in_use = self_frame_in_use_now();
	pushed_self_frame_in_use_ =
		pipeline_.set_assign_policy(settings_.assign_mode, std::move(always_on), std::move(voice_links),
		                            settings_.broadcaster_user_id, self_frame_selected(), in_use);
	if (in_use && !pushed_self_frame_in_use_)
		refuse_self_frame();
	// 受け取る範囲も同じ周期で渡す (設定を変えたら、OBS を再起動せずに反映される)。
	pipeline_.set_receive_scope(settings_.receive_scope);
}

void DockPoll::set_person_mute(const ledger::ActionArgs &args, pipeline::MixerKind kind)
{
	if (args.user_id.empty())
		return;
	pipeline_.set_person_audio_mute(args.user_id, kind, args.on);
	// メインアカウントの声のミュートだけは、settings.json にも書く (pipeline.h の set_person_audio_mute)。Pipeline の
	// 側でも自分の声は、メインアカウントの userId (push_assign_policy が渡した settings_ の値) で見分けている。
	const bool self_voice = kind == pipeline::MixerKind::Voice && !settings_.broadcaster_user_id.empty() &&
				args.user_id == settings_.broadcaster_user_id;
	if (self_voice && settings_.self_voice_muted != args.on) {
		settings_.self_voice_muted = args.on;
		write_settings();
	}
	diag::logf(LOG_INFO, "%s %s", kind == pipeline::MixerKind::Voice ? "Person voice" : "Person screen audio",
		   args.on ? "muted" : "unmuted");
}

void DockPoll::set_person_volume(const ledger::ActionArgs &args, pipeline::MixerKind kind)
{
	if (args.user_id.empty())
		return;
	const int pct = config::clamp_person_volume_pct(args.volume_pct);
	pipeline_.set_person_audio_volume(args.user_id, kind, pct);

	// 人の行に書く。100% に戻す操作で、行がまだなければ、行を作らない (書くものがない)。
	ledger::SeenPerson *person = nullptr;
	for (ledger::SeenPerson &row : seen_people_) {
		if (row.user_id == args.user_id) {
			person = &row;
			break;
		}
	}
	if (person == nullptr && pct != config::kPersonVolumeDefaultPct)
		person = &config::ensure_seen_person(seen_people_, args.user_id);
	if (person == nullptr)
		return;
	int &saved = kind == pipeline::MixerKind::Voice ? person->voice_volume_pct : person->screen_volume_pct;
	if (saved == pct)
		return;
	saved = pct;
	schedule_seen_flush(args.user_id, kind);
}

void DockPoll::push_person_volumes()
{
	std::map<std::string, pipeline::Pipeline::PersonVolumes> volumes;
	for (const ledger::SeenPerson &person : seen_people_) {
		if (person.user_id.empty())
			continue;
		if (person.voice_volume_pct == config::kPersonVolumeDefaultPct &&
		    person.screen_volume_pct == config::kPersonVolumeDefaultPct)
			continue;
		pipeline::Pipeline::PersonVolumes v;
		v.voice_pct = person.voice_volume_pct;
		v.screen_pct = person.screen_volume_pct;
		volumes.emplace(person.user_id, v);
	}
	pipeline_.set_person_volumes(volumes);
}

void DockPoll::schedule_seen_flush(const std::string &user_id, pipeline::MixerKind kind)
{
	(kind == pipeline::MixerKind::Voice ? pending_voice_volume_users_ : pending_screen_volume_users_)
		.insert(user_id);
	if (!seen_flush_timer_) {
		seen_flush_timer_ = std::make_unique<QTimer>();
		seen_flush_timer_->setSingleShot(true);
		seen_flush_timer_->setInterval(kSeenFlushDelayMs);
		QObject::connect(seen_flush_timer_.get(), &QTimer::timeout, seen_flush_timer_.get(),
				 [this] { flush_seen_volumes(); });
	}
	seen_flush_timer_->start(); // 動いていれば数え直す
}

void DockPoll::flush_seen_volumes()
{
	const std::size_t voice = pending_voice_volume_users_.size();
	const std::size_t screen = pending_screen_volume_users_.size();
	pending_voice_volume_users_.clear();
	pending_screen_volume_users_.clear();
	if (voice == 0 && screen == 0)
		return;
	write_seen_people();
	diag::logf(LOG_INFO, "Saved person volumes (voice: %zu, screen audio: %zu)", voice, screen);
}

void DockPoll::update_self_added_mark(const pipeline::PipelineSnapshot &snapshot)
{
	// 決めるのは、チェックが付いていて、自分の枠が実際に映っている時である。チェックが付いていても映っていない間 (映像を
	// 「Discordの画面共有」にしている間、選んだソースが見つからない間) に決めると、利用者に見えない時の並びで位置が決まる。
	// 映り始めた後に一時的に映らなくなっても (ソースが一時的に見つからない、シーンコレクションの切り替え)、チェックが付いて
	// いる間は決め直さない (位置が人の出入りと関係なく動かないように)。
	// 割り当て表の載せた順の番号は、載せるたびに大きくなる。映った時に載っていた人の最大を覚えれば、その人たちの後で、
	// それより後に載せた人の前になる (ledger/slot_cells.h の SlotCellSelf::added_mark)。番号を割り当て表から新しく取らない
	// のは、割り当て表が Pipeline のスレッドにあり、自分の枠は割り当て表の外にあるためである。映った直後に、まだ snapshot に
	// 入っていない人が載ると、その人より後になることがある (次のポーリングまでの間だけ)。
	std::int64_t max_order = 0;
	for (const pipeline::SlotSnapshot &row : snapshot.slots) {
		if (row.kind == config::SlotKind::Screen && !row.owner_user_id.empty())
			max_order = std::max(max_order, row.added_order);
	}
	// ダミー映像の間は自分の枠を描かない (sources/composite_input.h の dummy_composite_input) ので、映っていない間と同じに扱う。
	const bool shown = self_frame_shown_now() && !sources::dummy_mode_active();
	self_order_mark_ = sources::next_self_order_mark(self_order_mark_, self_frame_state_.on, shown, max_order);
}

bool DockPoll::self_frame_shown_now() const
{
	return self_frame_state_.on && sources::self_frame_shown();
}

bool DockPoll::self_frame_in_use_now() const
{
	return config::self_frame_in_use(self_frame_selected(), self_frame_state_.on);
}

void DockPoll::push_self_frame_in_use_if_changed()
{
	if (self_frame_in_use_now() != pushed_self_frame_in_use_)
		push_assign_policy();
}

void DockPoll::refuse_self_frame()
{
	sources::SelfFrameInput in;
	in.state = self_frame_state_;
	in.event = sources::SelfFrameEvent::Refused;
	const sources::SelfFrameResult result = sources::plan_self_frame(in);
	self_frame_state_ = result.state;
	if (settings_.self_frame_on != result.state.on) {
		settings_.self_frame_on = result.state.on;
		write_settings();
	}
	// layout_step の中で断られたときは、この後の配置の入力に自分の枠を入れない (入れると、この周期だけ 5 つになる)。
	self_frame_facts_.self_frame_on = result.state.on;
	self_frame_facts_.self_frame_shown = result.shown;
	diag::logf(LOG_INFO, "Self frame turned off: all screen slots are in use");
}

void DockPoll::step_self_frame(const pipeline::PipelineSnapshot &snapshot, bool bound, DockFacts *facts)
{
	// 自分の Discord の共有。facts の sharing を信じてよく、自分の枠の人が決まっているときだけ分かる。そうでなければ
	// 分からないままにし、「共有していない」とは扱わない。
	const std::string self_owner = self_frame_user_id(settings_, snapshot);
	sources::SelfShare share = sources::SelfShare::Unknown;
	if (snapshot.facts.received && snapshot.facts.sharing_valid && !self_owner.empty()) {
		const std::vector<std::string> &sharing = snapshot.facts.sharing.user_ids;
		share = std::find(sharing.begin(), sharing.end(), self_owner) != sharing.end() ? sources::SelfShare::On
												: sources::SelfShare::Off;
	}

	// 画面共有の枠を埋めている Discord の人の数。自分の枠の人は数えない。
	const int discord_slotted = discord_slotted_of(snapshot, self_owner);

	// 映す映像を OBS から選んでいるか。選んでいない間 (「Discord の画面共有」) は、ほかの人と同じ経路 (StreamRouter) が
	// メインアカウントの行を動かすので、ここでは on を変えない。
	const bool selected = self_frame_selected();
	bool always_on = false;
	for (const ledger::SeenPerson &person : seen_people_) {
		if (!self_owner.empty() && person.user_id == self_owner && person.always_on) {
			always_on = true;
			break;
		}
	}

	sources::SelfFrameInput in;
	in.state = self_frame_state_;
	in.share = share;
	in.event = sources::SelfFrameEvent::Poll;
	in.selected = selected;
	in.bound = bound;
	in.automatic = settings_.assign_mode == config::AssignMode::Auto;
	in.always_on = always_on;
	in.discord_slotted = discord_slotted;
	in.slot_count = config::kScreenSlotCount;
	const sources::SelfFrameResult result = sources::plan_self_frame(in);

	if (result.persist) {
		diag::logf(LOG_INFO, "Self frame turned %s: screen share", result.state.on ? "on" : "off");
		settings_.self_frame_on = result.state.on;
		write_settings();
	}
	// 付けるはずの枠が埋まっている間は、周期ごとに同じ結果になる。ログは見送り始めたときの 1 回だけ書く。
	if (result.deferred && !self_frame_deferred_logged_)
		diag::logf(LOG_INFO, "Self frame not turned on: all screen slots are in use");
	self_frame_deferred_logged_ = result.deferred;
	self_frame_state_ = result.state;

	facts->self_frame_bound = bound;
	facts->self_frame_on = result.state.on;
	facts->self_frame_shown = result.shown;
	facts->self_frame_share = share == sources::SelfShare::On    ? ledger::SelfShareState::On
				  : share == sources::SelfShare::Off ? ledger::SelfShareState::Off
								     : ledger::SelfShareState::Unknown;
}

void DockPoll::set_self_frame(bool on)
{
	// ドックのチェックの操作。共有の始まりと終わりは見ない (直前の共有の値は変えない)。on が変わったら settings.json に
	// 残す。
	sources::SelfFrameInput in;
	in.state = self_frame_state_;
	in.event = on ? sources::SelfFrameEvent::DockOn : sources::SelfFrameEvent::DockOff;
	const sources::SelfFrameResult result = sources::plan_self_frame(in);
	self_frame_state_ = result.state;
	if (result.persist) {
		settings_.self_frame_on = result.state.on;
		write_settings();
	}
	diag::logf(LOG_INFO, "Self frame turned %s: dock", on ? "on" : "off");
	// 次の周期を待たずに、画面の枠に載せられる人数を渡し直す (付けた直後に、自動で 4 人目が載らないように)。押せる判断の後に
	// 自動で 4 人目が載っていれば、Pipeline が断り、チェックを外す (refuse_self_frame)。
	push_self_frame_in_use_if_changed();
}

bool DockPoll::self_frame_selected() const
{
	return config::self_frame_video_selected(settings_);
}

void DockPoll::carry_self_frame_check(bool now_selected)
{
	// 自分の枠に映す映像を替えたときに、メインアカウントのチェックを引き継ぐ。起動の途中の結び付きでは呼ばない。
	// サブアカウントがない構成 (自分の枠の人が Vesktop のアカウント) には、Discord の画面共有を枠に載せる経路がないので、
	// 引き継ぐものがない。
	const pipeline::PipelineSnapshot snapshot = pipeline_.snapshot();
	const std::string owner = self_frame_user_id(settings_, snapshot);
	const bool has_sub_account =
		!settings_.broadcaster_user_id.empty() && settings_.broadcaster_user_id != snapshot.facts.self_user_id.value_or("");
	const bool carries = !owner.empty() && has_sub_account;

	if (now_selected) {
		// Discord の画面共有からメインアカウントを枠に載せていたら、そのチェックを自分の枠のチェックに移す。
		int slot = 0;
		std::int64_t order = 0;
		for (const pipeline::SlotSnapshot &row : snapshot.slots) {
			if (carries && row.kind == config::SlotKind::Screen && row.owner_user_id == owner) {
				slot = row.slot;
				order = row.added_order;
			}
		}
		if (slot == 0) {
			// 前から付いていたチェック (「Discordの画面共有」か「(なし)」に替えたときに、メインアカウントを枠に載せられずに
			// 残したチェック) は、そのまま自分の枠のチェックになる。その間に Discord の人が枠を 4 つとも埋めていたら、自分の枠を
			// 足すと 5 つになり、1 つを描けないので、この後の push_assign_policy で Pipeline が断り、チェックを外す
			// (refuse_self_frame。ドックで付けられないのと同じ規則)。外すだけで excluded にはしないので、ほかの人と同じ規則で
			// 後から付く (自動なら枠が空いたとき)。
			return;
		}
		self_frame_state_.on = true;
		self_frame_state_.excluded = false;
		settings_.self_frame_on = true;
		// 載せた順の位置は、メインアカウントの行にチェックを付けた時のまま引き継ぐ (映像の種類を替えても並びの位置を変えない)。
		// 自分の枠を、メインアカウントの枠の番号 order の人がいた位置 (order - 1 の人の後、order + 1 の人の前) に置く。
		self_order_mark_ = sources::self_order_mark_from_share(order);
		diag::logf(LOG_INFO, "Self frame turned on: video changed");
		// メインアカウントの枠を空けるのと、自分の枠を使い始めるのを、Pipeline の 1 回のロックの中で行う
		// (Pipeline::move_self_frame)。分けると、その間に自動で待っていた人が空いた枠に載り、自分の枠を使い始められなくなる。
		const pipeline::Pipeline::SelfFrameMove moved = pipeline_.move_self_frame(owner, true);
		pushed_self_frame_in_use_ = moved.self_frame_in_use;
		if (moved.change.outcome == pipeline::Pipeline::SlotChange::Outcome::Released) {
			diag::logf(LOG_INFO, "Removed a person from slot %d", moved.change.slot);
			after_slot_release(config::SlotKind::Screen, moved.change.slot);
		} else {
			// スナップショットの後に、メインアカウントが画面共有をやめて枠が空いていた。
			diag::logf(LOG_INFO, "Couldn't remove the person from a slot: they're no longer in one");
		}
		// 空けた枠の分で使うので普通は断られないが、メインアカウントの枠がスナップショットの後に空いて、自動で待っていた人が
		// 載っていれば断られる。
		if (!moved.self_frame_in_use)
			refuse_self_frame();
		return;
	}

	// 自分の枠のチェックが付いていたら、Discord の画面共有の枠に載せる。枠が空いていないか、その人が画面共有を
	// していなければ、チェックは残す (画面共有していない人は枠に載せられない)。
	// 自分の枠を使うのをやめるのと、メインアカウントを載せるのを、Pipeline の 1 回のロックの中で行う
	// (Pipeline::move_self_frame)。自分の枠を使っている間に載せると上限 (3 人) で断られ、先にやめると、空いた 4 つ目の枠に
	// 自動で待っていた人が先に載る。
	if (!carries || !self_frame_state_.on)
		return;
	if (snapshot.facts.sharing_valid) {
		const std::vector<std::string> &sharing = snapshot.facts.sharing.user_ids;
		if (std::find(sharing.begin(), sharing.end(), owner) == sharing.end()) {
			diag::logf(LOG_INFO, "Self frame check kept: the account isn't sharing its screen");
			return;
		}
	}
	// 載せた順の位置は、自分の枠の位置を引き継ぐ (自分の枠を載せた順の印の人の後に置く。印をまだ決めていなければ、ほかの人と
	// 同じく今の最後)。
	const std::optional<std::int64_t> after =
		self_order_mark_.decided ? std::optional<std::int64_t>(self_order_mark_.mark) : std::nullopt;
	const pipeline::Pipeline::SelfFrameMove moved = pipeline_.move_self_frame(owner, false, after);
	pushed_self_frame_in_use_ = moved.self_frame_in_use;
	const pipeline::Pipeline::SlotChange &change = moved.change;
	if (change.outcome == pipeline::Pipeline::SlotChange::Outcome::Assigned ||
	    change.outcome == pipeline::Pipeline::SlotChange::Outcome::AlreadyOwned) {
		self_frame_state_.on = false;
		settings_.self_frame_on = false;
		diag::logf(LOG_INFO, "Self frame turned off: video changed to the screen share");
	} else {
		diag::logf(LOG_INFO, "Self frame check kept: no slot is free for the screen share");
	}
}

void DockPoll::replace_slot(const ledger::ActionArgs &args)
{
	const pipeline::Pipeline::SlotChange change =
		pipeline_.replace_slot(config::SlotKind::Screen, args.replace_user_id, args.user_id);
	if (change.outcome != pipeline::Pipeline::SlotChange::Outcome::Replaced) {
		// 押せる条件は ledger::apply が先に確認しているので、ここに来るのは、ポーリングの間に割り当てが
		// 変わったときだけである。
		diag::logf(LOG_INFO, "Couldn't replace the person in the slot: the slots have changed");
		return;
	}
	diag::logf(LOG_INFO, "Replaced the person in slot %d", change.slot);
	if (ops_.last_slot_refused_user_id == args.user_id)
		ops_.last_slot_refused_user_id.clear();
	// 外した人の画面と名前ラベルを枠に残さない。載せた人の名前ラベルは、次のポーリングが渡す。
	after_slot_release(config::SlotKind::Screen, change.slot);
}

void DockPoll::set_main_account(const std::string &user_id)
{
	if (settings_.broadcaster_user_id == user_id)
		return;
	settings_.broadcaster_user_id = user_id;
	write_settings();
	// 設定ウィンドウの [適用] と同じ後処理。自分の声の扱い (Pipeline の set_assign_policy) と、自分の枠の人と名前ラベル
	// (DockPollLayoutPart) が、次のポーリングを待たずに新しい人で動く。
	push_assign_policy();
	DockPollLayoutPart::on_settings_changed(*this);
	DockPollLayoutPart::distribute_person_decor(*this, pipeline_.snapshot());
	diag::logf(LOG_INFO, "%s", user_id.empty() ? "Main account cleared" : "Main account changed");
}

void DockPoll::set_slot(const ledger::ActionArgs &args)
{
	// 枠の番号の 0 は枠から外すこと、1 以上は枠に載せることを表す (ledger/actions.h の ActionArgs::slot)。
	// どちらも割り当て表を書くので、Pipeline の関数を通してだけ行う。
	if (args.slot == 0) {
		const pipeline::Pipeline::SlotChange change = pipeline_.release_slot(args.kind, args.user_id);
		if (change.outcome != pipeline::Pipeline::SlotChange::Outcome::Released) {
			// 押せる条件は ledger::apply が先に確認しているので、ここに来るのは、ポーリングの間に割り当てが
			// 変わったときだけである。何もしないときも、ログに書く。
			diag::logf(LOG_INFO, "Couldn't remove the person from a slot: they're no longer in one");
			return;
		}
		diag::logf(LOG_INFO, "Removed a person from slot %d", change.slot);
		after_slot_release(args.kind, change.slot);
		return;
	}

	// 載せる枠を決めるのは割り当て表 (try_assign) である。画面に表示した枠の番号 (args.slot。空いている最も小さい
	// 番号の枠) と同じ答えになるが、最後に決めるのは割り当て表にする。割り当てを残す規則を、ドックが別に持たない
	// ためである。
	const pipeline::Pipeline::SlotChange change = pipeline_.assign_slot(args.kind, args.user_id);
	switch (change.outcome) {
	case pipeline::Pipeline::SlotChange::Outcome::Assigned:
		diag::logf(LOG_INFO, "Added a person to slot %d", change.slot);
		if (ops_.last_slot_refused_user_id == args.user_id)
			ops_.last_slot_refused_user_id.clear();
		break;
	case pipeline::Pipeline::SlotChange::Outcome::AlreadyOwned:
		diag::logf(LOG_INFO, "The person was already in slot %d", change.slot);
		if (ops_.last_slot_refused_user_id == args.user_id)
			ops_.last_slot_refused_user_id.clear();
		break;
	case pipeline::Pipeline::SlotChange::Outcome::Insufficient:
	case pipeline::Pipeline::SlotChange::Outcome::Released:
	case pipeline::Pipeline::SlotChange::Outcome::NotAssigned:
		diag::logf(LOG_WARNING, "Couldn't add the person to a slot: no slot is free");
		// 押した直後に空きがなくなって、載せられなかった (競合。自分の枠を使っている間に、自動で載せる経路 (選び方が「自動」の
		// とき、[自動で枠に載せる] の人) が 3 人目を載せていたときも含む。Pipeline::assign_slot)。押した所の近く (その人の行) に
		// 表示するために記録する。消すのは、その人が載ったとき (ここと replace_slot) だけである (別の人が載せられなかったときは、
		// その人に替わる)。空きができても消さず、空いている枠がある間は、ledger が表示しない (ledger/rules_dock.cpp の refused_note)。
		ops_.last_slot_refused_user_id = args.user_id;
		break;
	}
}

} // namespace cordmux::dock
