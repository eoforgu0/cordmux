// SPDX-License-Identifier: Apache-2.0

// sim_poll.h の実装。

#include "sim_poll.h"

#include <obs-module.h>

#include <QMainWindow>
#include <QWidget>

#include <algorithm>
#include <map>
#include <set>

#include "dock/audio_tab.h"
#include "dock/dock_widget.h"
#include "dock/look_tab.h"
#include "dock/people_list.h"
#include "dock/settings_fields.h"
#include "dock/ui/text_resolve.h"
#include "drive_env.h"
#include "ledger/look_tab.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"
#include "obs_dock.h"
#include "pipeline/pipeline.h"

namespace cordmux::dock_drive {

namespace {

ledger::PersonFact *person_of(ledger::Observation &obs, const std::string &user_id)
{
	for (ledger::PersonFact &p : obs.people) {
		if (p.user_id == user_id)
			return &p;
	}
	return nullptr;
}

ledger::SeenPerson &seen_of(ledger::Observation &obs, const std::string &user_id)
{
	for (ledger::SeenPerson &p : obs.seen.people) {
		if (p.user_id == user_id)
			return p;
	}
	ledger::SeenPerson added;
	added.user_id = user_id;
	if (const ledger::PersonFact *p = person_of(obs, user_id); p != nullptr && p->display_name.has_value())
		added.display_name = *p->display_name;
	obs.seen.people.push_back(added);
	return obs.seen.people.back();
}

} // namespace

SimPoll::SimPoll(const ui_preview::Fixture &base, const dock::ui::DockStyle &style)
{
	obs_ = ui_preview::observation_for(base);
	settings_ = base.settings;
	status_state_ = base.status;
	base_now_us_ = obs_.now_us;
	clock_.start();

	// OBS の obs_frontend_add_dock_by_id と同じく OBSDock で包む (tests/ui_preview/render.cpp の build_dock と同じ
	// 形)。プレビュー用のプラットフォームプラグインの上なので、show しても OS のウィンドウは表示されない。
	// WA_DontShowOnScreen は付けない。付けると Qt のウィンドウが作られず、入力の送り先がなくなる。
	dock_ = std::make_unique<OBSDock>();
	dock_->setObjectName(QStringLiteral("cordmux"));
	dock_->setWindowTitle(QString::fromUtf8(obs_module_text(ledger::text::keys::kDockWindowTitle)));
	dock_->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			   QDockWidget::DockWidgetFloatable);
	widget_ = new dock::DockWidget();
	dock_->setWidget(widget_);
	// OBS と同じく、メインウィンドウのドックにしてからフローティングにする (OBSBasic::AddDockWidget の後、
	// 最初はフローティング。dock/dock_bind.cpp)。メインウィンドウは、親ウィンドウのつながりを実際と同じにするのに
	// 必要である。ドックのダイアログの WindowModal がどこまで入力を止めるかと、設定ウィンドウの失敗のダイアログが
	// メインウィンドウとドックを止めないかを、このつながりで確認する。
	if (auto *main = qobject_cast<QMainWindow *>(main_window())) {
		main->addDockWidget(Qt::RightDockWidgetArea, dock_.get());
		dock_->setFloating(true);
	}
	widget_->set_style(style);
	widget_->set_action_callback([this](ledger::ActionId id, const ledger::ActionArgs &args) { run_action(id, args); });
	widget_->set_rename_callback([this](const std::string &user_id) {
		if (on_rename)
			on_rename(user_id);
	});
	widget_->set_on_tab_changed([this](config::DockTab tab) {
		settings_.dock_tab = tab;
		tick_soon();
	});
	// 表示タブ。製品の DockPoll と同じく、値を変えたらすぐ設定に入れ、保存は変更の少し後にまとめる。
	saved_settings_ = settings_;
	look_save_ = std::make_unique<dock::DeferredSave>(ledger::kLookSaveDelayMs, [this] {
		++look_saves_;
		saved_settings_ = settings_;
		look_events_.push_back({elapsed_ms(), 's'});
	});
	widget_->set_look_changed_callback([this](const config::Settings &tab) { on_look_changed(tab); });
	widget_->set_buffer_changed_callback([this](int ms) { on_buffer_changed(ms); });
}

void SimPoll::on_buffer_changed(int ms)
{
	// 映像のバッファだけを変える (dock/dock_poll_settings.cpp の DockPoll::on_buffer_changed と同じ)。
	++buffer_reflects_;
	config::Settings from = settings_;
	from.display_buffer_ms = ms;
	on_look_changed(from);
}

void SimPoll::on_look_changed(const config::Settings &from_tab)
{
	// 表示タブの項目だけを入れる (範囲は clamp_settings で収めてから戻す。dock/dock_poll_settings.cpp の
	// DockPoll::on_look_changed と同じ)。設定ウィンドウの値と ActionId を通る値は書き換えない。
	config::Settings next = settings_;
	dock::apply_look_tab_fields(from_tab, next);
	config::clamp_settings(next);
	dock::apply_look_tab_fields(next, settings_);
	// 行の枠の位置を決める設定は、製品では DockPoll が周期ごとに設定から Observation に入れる (DockPoll::current_facts)。
	// ここでは変えたときに入れる。
	obs_.choices.slot_order = settings_.slot_order;
	obs_.choices.self_slot_first = settings_.self_slot_first;
	++look_reflects_;
	look_events_.push_back({elapsed_ms(), 'r'});
	if (on_look_reflect)
		on_look_reflect(settings_);
	look_save_->touch();
}

void SimPoll::flush_look()
{
	widget_->flush_look();
	if (look_save_)
		look_save_->flush();
}

SimPoll::~SimPoll()
{
	stop();
	if (level_timer_ && widget_ != nullptr)
		widget_->set_on_levels_wanted(nullptr);
	level_timer_.reset();
	if (widget_ != nullptr) {
		widget_->set_action_callback(nullptr);
		widget_->set_on_tab_changed(nullptr);
		widget_->set_look_changed_callback(nullptr);
		widget_->set_buffer_changed_callback(nullptr);
	}
	look_save_.reset();
}

dock::PeopleList *SimPoll::people() const
{
	for (QWidget *c : widget_->findChildren<QWidget *>()) {
		if (auto *p = dynamic_cast<dock::PeopleList *>(c))
			return p;
	}
	return nullptr;
}

dock::AudioTab *SimPoll::audio() const
{
	for (QWidget *c : widget_->findChildren<QWidget *>()) {
		if (auto *p = dynamic_cast<dock::AudioTab *>(c))
			return p;
	}
	return nullptr;
}

void SimPoll::enable_level_timer()
{
	if (level_timer_)
		return;
	level_timer_ = std::make_unique<QTimer>();
	level_timer_->setInterval(100);
	QObject::connect(level_timer_.get(), &QTimer::timeout, level_timer_.get(), [this] {
		++level_reads_;
		widget_->set_speaking(levels_.speaking);
		widget_->set_audio_levels(levels_.tracks);
	});
	widget_->set_on_levels_wanted([this](bool wanted) {
		if (wanted)
			level_timer_->start();
		else
			level_timer_->stop();
	});
	if (widget_->levels_wanted())
		level_timer_->start();
}

std::int64_t SimPoll::now_us() const
{
	return base_now_us_ + clock_.nsecsElapsed() / 1000;
}

void SimPoll::start(int poll_ms)
{
	if (timer_)
		return;
	timer_ = std::make_unique<QTimer>();
	timer_->setInterval(poll_ms);
	QObject::connect(timer_.get(), &QTimer::timeout, timer_.get(), [this] { tick(); });
	timer_->start();
	tick();
}

void SimPoll::stop()
{
	if (timer_) {
		timer_->stop();
		timer_.reset();
	}
}

void SimPoll::tick_soon()
{
	if (timer_)
		QTimer::singleShot(0, timer_.get(), [this] { tick(); });
	else
		tick();
}

void SimPoll::use_product_rename_dialog()
{
	widget_->set_rename_callback(nullptr);
}

void SimPoll::set_frames_stopped(const std::string &user_id, bool stopped)
{
	if (stopped)
		stopped_frames_.insert(user_id);
	else
		stopped_frames_.erase(user_id);
	changed();
}

void SimPoll::set_busy(const std::set<ledger::ActionId> &busy)
{
	busy_ = busy;
}

void SimPoll::tick()
{
	// 周期の処理の中から周期の処理を呼ばない (メニューの入れ子のイベントループで、0 ms の周期が重なったとき)。
	if (in_tick_)
		return;
	in_tick_ = true;
	++ticks_;
	const std::int64_t now = now_us();
	obs_.now_us = now;
	release_departed();
	// 枠に載っていて画面共有している人のフレームは届き続ける (止めた人を除く)。実際には、
	// Pipeline のスナップショットが周期ごとに新しい時刻を持つ。
	for (ledger::PersonFact &p : obs_.people) {
		if (p.slot.has_value() && p.sharing && stopped_frames_.count(p.user_id) == 0)
			p.last_video_frame_at_us = now - 100'000;
	}
	obs_.first_frame_wait_threshold_us = static_cast<std::int64_t>(settings_.first_frame_wait_ms) * 1000;
	// 直近 1 分の列は、状態タブが見えている間だけ入る (DockPoll::tick の set_arrival_watch)。
	obs_.receive.arrival = widget_->status_tab_open() ? arrival_ : std::vector<ledger::ArrivalFact>{};
	if (poll_self_frame_)
		step_self_frame(sources::SelfFrameEvent::Poll);
	// 製品では、配置の入力を渡す処理 (layout_step) が tick の前に全画面を決め直す。
	settle_focus();

	// 判断 (純粋関数)、表示と消去の待ち、描画の順に行う (DockPoll::tick と同じ順)。
	raw_ = ledger::decide_dock(obs_);
	ledger::NoticeTrackerConfig cfg;
	cfg.show_delay_us = static_cast<std::int64_t>(settings_.notice_show_delay_ms) * 1000;
	cfg.clear_delay_us = static_cast<std::int64_t>(settings_.notice_clear_delay_ms) * 1000;
	ledger::StabilizeResult stable = ledger::stabilize(tracker_, raw_, now, cfg);
	tracker_ = stable.next;
	ledger::FirstLineInput line_input;
	line_input.now_us = now;
	line_input.streaming = obs_.uninstall.obs_streaming.value_or(false);
	line_input.status_tab_open = widget_->status_tab_open();
	ledger::FirstLineResult settled = ledger::settle_first_line(read_state_, stable.view, line_input);
	read_state_ = settled.next;
	stable.view.first_line = settled.first_line;
	// 状態タブ。累計と履歴は、タブが見えていなくても進める。
	ledger::StatusInput status_input;
	status_input.now_us = now;
	status_input.utc_offset_sec = obs_.utc_offset_sec;
	status_input.streaming = line_input.streaming;
	ledger::StatusResult status = ledger::settle_status(status_state_, stable.view, obs_, status_input);
	status_state_ = std::move(status.next);
	stable.view.status_tab = std::move(status.view);
	view_ = stable.view;
	widget_->set_confirm_inputs(line_input.streaming, settings_.confirm_while_streaming);
	widget_->set_look_values(settings_);
	widget_->set_layout_hold_idle_ms(settings_.layout_hold_idle_ms);
	widget_->set_status_tab_label(settled.status_tab_label);
	widget_->render(stable.view);
	widget_->set_busy_actions(busy_);
	notice_log_lines_ += static_cast<std::int64_t>(stable.diff.appeared.size() + stable.diff.disappeared.size());
	announce(stable.diff, stable.view.first_line);
	rendered_people_ = !stable.view.people.empty();
	in_tick_ = false;
	if (on_tick)
		on_tick(stable);
}

void SimPoll::release_departed()
{
	// 接続していない間は facts が届かず、通話の参加者がいない間 (Vesktop のアカウントが通話の外) は、やめたとは分からない。
	if (obs_.link.state != ledger::LinkState::Connected || obs_.call.presence == ledger::Presence::NotInCall)
		return;
	bool roster_known = false;
	for (const ledger::PersonFact &p : obs_.people)
		roster_known = roster_known || p.in_roster;
	if (!roster_known)
		return;
	for (ledger::PersonFact &p : obs_.people) {
		if (p.slot.has_value() && ((p.sharing_known && !p.sharing) || !p.in_roster))
			p.slot.reset();
	}
}

void SimPoll::settle_focus()
{
	ledger::Choices &c = obs_.choices;
	// ダミー映像の表示の間は、製品はダミーのタイルで全画面を決める。ここでは再現しないので、前の値のままにする。
	if (c.dummy == ledger::FeatureSwitch::On)
		return;
	const std::vector<sources::CompositeFocusTile> tiles = ui_preview::focus_tiles_for(obs_);
	const std::string kept = sources::focus_after_layout(c.focus_user_id, tiles, c.feature != ledger::FeatureSwitch::Off);
	if (kept != c.focus_user_id)
		changed();
	c.focus_user_id = kept;
	c.focus_name.clear();
	c.focus_candidates.clear();
	for (const sources::CompositeFocusTile &tile : tiles) {
		c.focus_candidates.push_back(tile.user_id);
		if (!kept.empty() && tile.user_id == kept && c.focus_name.empty())
			c.focus_name = tile.name;
	}
}

void SimPoll::step_self_frame(sources::SelfFrameEvent event)
{
	ledger::Choices &c = obs_.choices;
	sources::SelfFrameInput in;
	self_frame_.on = c.self_frame_on;
	in.state = self_frame_;
	in.event = event;
	in.share = c.self_frame_share == ledger::SelfShareState::On    ? sources::SelfShare::On
		   : c.self_frame_share == ledger::SelfShareState::Off ? sources::SelfShare::Off
								      : sources::SelfShare::Unknown;
	in.selected = c.self_frame_selected;
	in.bound = c.self_frame_bound;
	in.automatic = c.assign_mode == ledger::AssignMode::Auto;
	for (const ledger::PersonFact &p : obs_.people) {
		if (p.slot.has_value() && !p.is_self_frame)
			++in.discord_slotted;
		if (p.is_self_frame) {
			for (const ledger::SeenPerson &seen : obs_.seen.people) {
				if (seen.user_id == p.user_id && seen.always_on)
					in.always_on = true;
			}
		}
	}
	in.slot_count = config::kScreenSlotCount;
	const sources::SelfFrameResult result = sources::plan_self_frame(in);
	self_frame_ = result.state;
	if (result.deferred)
		++self_frame_deferred_;
	if (result.persist)
		settings_.self_frame_on = result.state.on;
	if (c.self_frame_on != result.state.on || c.self_frame_shown != result.shown) {
		c.self_frame_on = result.state.on;
		c.self_frame_shown = result.shown;
		changed();
	}
}

void SimPoll::announce(const ledger::NoticeDiff &diff, const ledger::dockview::FirstLine &first_line)
{
	// 何を通知するかは、製品の dock::announcements_for (DockPoll::report_notice_changes と同じ関数) をそのまま
	// 呼んで決める。決め方を再現すると、製品の決め方が変わったときに、テストが古い決め方のまま確認してしまう。
	// 一覧が前から表示されていたか (list_was_shown) は、DockPoll の rendered_people_ と同じく、前の周期に一覧を
	// 描いたかで決める。
	for (const dock::Announcement &a : dock::announcements_for(diff, first_line, rendered_people_))
		widget_->announce(a.text, a.assertive);
}

void SimPoll::run_action(ledger::ActionId id, const ledger::ActionArgs &args, bool from_rename)
{
	// 押せるかを決めるのは ledger::apply() だけ (製品の DockPoll::run_action と同じ)。
	const ledger::Intent intent = ledger::apply(obs_, id, args);
	ActionRecord rec;
	rec.t_ms = elapsed_ms();
	rec.id = id;
	rec.args = args;
	rec.kind = intent.kind;
	rec.ran = intent.can_run();
	rec.from_rename = from_rename;
	actions_.push_back(rec);
	bool handled = false;
	if (on_action)
		handled = on_action(rec);
	if (!handled && intent.can_run())
		apply_intent(intent);
	// 操作の結果をすぐに画面に反映する (0 ms の後に周期の処理を 1 回行う。DockPoll::run_action の最後と同じ)。
	tick_soon();
}

void SimPoll::apply_intent(const ledger::Intent &intent)
{
	const ledger::ActionArgs &a = intent.args;
	switch (intent.kind) {
	case ledger::IntentKind::SetSlot: {
		if (a.kind != config::SlotKind::Screen)
			break; // 声の枠は人の slot に入らない (slot は画面共有の枠だけ)。記録だけにする
		ledger::PersonFact *p = person_of(obs_, a.user_id);
		if (p == nullptr)
			break;
		if (a.slot > 0) {
			// 製品は、枠に載せると視聴の範囲に入れ、その時刻を覚える (Pipeline::Impl::note_watch_since_locked)。「枠に載せた
			// 人だけ」のときだけ持つ。
			if (!p->slot.has_value() && obs_.choices.receive_scope == ledger::ReceiveScope::Slots)
				p->watching_since_us = now_us();
			p->slot = a.slot;
		} else {
			p->slot.reset();
			p->watching_since_us.reset();
			// 通話から抜けた後も枠に載っていた人は、枠から外すと一覧から消える
			// (dock/build_observation.cpp の people_of は、通話に参加していない人を、枠に載っている間だけ一覧に入れる)。
			if (!p->in_roster) {
				const std::string uid = p->user_id;
				obs_.people.erase(std::remove_if(obs_.people.begin(), obs_.people.end(),
								 [&](const ledger::PersonFact &x) { return x.user_id == uid; }),
						  obs_.people.end());
			}
		}
		changed();
		break;
	}
	case ledger::IntentKind::ReplaceSlot: {
		ledger::PersonFact *added = person_of(obs_, a.user_id);
		ledger::PersonFact *removed = person_of(obs_, a.replace_user_id);
		if (added != nullptr && removed != nullptr) {
			added->slot = removed->slot;
			removed->slot.reset();
			removed->watching_since_us.reset();
			if (obs_.choices.receive_scope == ledger::ReceiveScope::Slots)
				added->watching_since_us = now_us();
			changed();
		}
		break;
	}
	case ledger::IntentKind::SetAlwaysOn:
		seen_of(obs_, a.user_id).always_on = a.on;
		changed();
		break;
	case ledger::IntentKind::SetRosterShown:
		seen_of(obs_, a.user_id).roster_hidden = !a.on;
		changed();
		break;
	case ledger::IntentKind::SetVoiceMute:
	case ledger::IntentKind::SetScreenAudioMute:
	case ledger::IntentKind::SetVoiceVolume:
	case ledger::IntentKind::SetScreenAudioVolume: {
		// 参加者ごとの音量とミュート。製品では Pipeline の表に入り、次のスナップショットで人の audio に戻る
		// (dock/build_observation.cpp の people_of)。音量は seen_people.json の人の行にも書く。
		ledger::PersonFact *p = person_of(obs_, a.user_id);
		if (intent.kind == ledger::IntentKind::SetVoiceVolume) {
			seen_of(obs_, a.user_id).voice_volume_pct = a.volume_pct;
			if (p != nullptr)
				p->audio.voice_volume_pct = a.volume_pct;
		} else if (intent.kind == ledger::IntentKind::SetScreenAudioVolume) {
			seen_of(obs_, a.user_id).screen_volume_pct = a.volume_pct;
			if (p != nullptr)
				p->audio.screen_volume_pct = a.volume_pct;
		} else if (p != nullptr) {
			(intent.kind == ledger::IntentKind::SetVoiceMute ? p->audio.voice_muted : p->audio.screen_muted) = a.on;
		}
		changed();
		break;
	}
	case ledger::IntentKind::SetNameOverride: {
		ledger::SeenPerson &s = seen_of(obs_, a.user_id);
		if (a.name.empty())
			s.name_override.reset();
		else
			s.name_override = a.name;
		if (ledger::PersonFact *p = person_of(obs_, a.user_id); p != nullptr) {
			if (!a.name.empty())
				p->display_name = a.name;
			else if (p->discord_name.has_value())
				p->display_name = p->discord_name;
		}
		changed();
		break;
	}
	case ledger::IntentKind::SetVoiceSource: {
		ledger::SeenPerson &s = seen_of(obs_, a.user_id);
		if (a.voice_user_id.empty() || a.voice_user_id == a.user_id)
			s.voice_source_user_id.reset();
		else
			s.voice_source_user_id = a.voice_user_id;
		changed();
		break;
	}
	case ledger::IntentKind::SetSelfFrame:
		step_self_frame(a.on ? sources::SelfFrameEvent::DockOn : sources::SelfFrameEvent::DockOff);
		break;
	case ledger::IntentKind::ResetStallTally:
		status_state_ = ledger::reset_status(status_state_);
		break;
	case ledger::IntentKind::SetFocus:
		// 全画面の人を替える (DockPoll::set_focus)。全画面の人の名前も、ここで決め直す。
		obs_.choices.focus_user_id = a.user_id;
		settle_focus();
		changed();
		break;
	case ledger::IntentKind::SetFeature:
		settings_.feature_enabled = a.on;
		obs_.choices.feature = a.on ? ledger::FeatureSwitch::On : ledger::FeatureSwitch::Off;
		changed();
		break;
	case ledger::IntentKind::SetDummy:
		obs_.choices.dummy = a.on ? ledger::FeatureSwitch::On : ledger::FeatureSwitch::Off;
		changed();
		break;
	case ledger::IntentKind::SetMainAccount:
		// 設定のメインアカウントを書き換える (DockPoll::set_main_account)。設定ウィンドウの [適用] は、この値を上書きしない。
		settings_.broadcaster_user_id = a.user_id;
		obs_.choices.broadcaster_user_id = a.user_id;
		changed();
		break;
	case ledger::IntentKind::SetRecording:
		// 録画の段階はシナリオが決める。ここでは選んだ状態だけを反映する (段階を設定しない間は
		// Unknown のまま)。
		obs_.choices.recording = a.on ? ledger::FeatureSwitch::On : ledger::FeatureSwitch::Off;
		changed();
		break;
	default:
		// ウィンドウを開く操作、インストールの操作、エクスポートなどは記録だけにする (シナリオが
		// on_action で扱う)。
		break;
	}
}

} // namespace cordmux::dock_drive
