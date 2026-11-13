// SPDX-License-Identifier: Apache-2.0

// DockPoll の処理のうち、OBS の中の合成のソースと名前ラベルを扱うもの (配置の入力、全画面、名前ラベルに渡す値)。宣言は
// dock_poll.h と dock_poll_layout.h にある。

#include "dock/dock_poll.h"

#include <obs-module.h>

#include <QMetaObject>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "diag/diag_log.h"
#include "sources/composite_input.h"
#include "sources/composite_source.h"
#include "sources/decor_registry.h"
#include "sources/decor_params.h"
#include "sources/dummy_feed.h"
#include "sources/scene_names.h"
#include "sources/screen_end.h"
#include "sources/screen_feed.h"
#include "sources/self_frame_source.h"
#include "sources/self_speaking_source.h"

namespace cordmux::dock {

namespace {

// UI スレッドで run を実行するように、呼び出しを積む。どのスレッドから呼んでもよく、取るのはブリッジの mu だけである。
// flag が既に true なら積まない。
void post_to_ui(DockPollLayoutBridge &bridge, std::atomic<bool> &flag, void (*run)(DockPoll &))
{
	std::lock_guard<std::mutex> lock(bridge.mu);
	if (bridge.context == nullptr || bridge.owner == nullptr)
		return;
	if (flag.exchange(true))
		return; // 既に積んである。UI スレッドで flag を false に戻してから、次を積む
	DockPoll *owner = bridge.owner;
	QMetaObject::invokeMethod(bridge.context, [owner, run] { run(*owner); }, Qt::QueuedConnection);
}

} // namespace

DockPollLayoutState::DockPollLayoutState() : bridge(std::make_shared<DockPollLayoutBridge>()) {}

DockPollLayoutState::~DockPollLayoutState()
{
	std::lock_guard<std::mutex> lock(bridge->mu);
	bridge->context = nullptr;
	bridge->owner = nullptr;
}

void DockPoll::push_layout_now(const pipeline::PipelineSnapshot &snapshot, bool self_frame_shown)
{
	// シーンの読み込みが終わる前と、シーンコレクションの切り替えの途中は何もしない。これから破棄される
	// 合成のソースに渡さないためである。全画面も、その間はそのまま続ける。
	if (!loading_finished_ || snapshot.frozen)
		return;
	// 合成のソースがないシーンコレクションには、渡す先がない。誰も描かれないので、全画面にできる人もいない。
	if (!sources::composite_present()) {
		DockPollLayoutPart::settle_focus(*this, {}, {});
		return;
	}
	update_self_added_mark(snapshot);
	DockPollLayoutPart::push_composite_input(*this, snapshot, self_frame_shown);
}

void DockPoll::set_focus(const std::string &user_id)
{
	if (focus_user_id_ == user_id)
		return;
	// ログには人の ID と名前を書かない (どの枠かは、合成のソースの配置のログの行に出る)。
	diag::logf(LOG_INFO, "%s", user_id.empty()                ? "Full screen ended"
				   : focus_user_id_.empty()       ? "Full screen started"
								  : "Full screen switched to another person");
	focus_user_id_ = user_id;
	// 全画面でないときの配置に描かれていない人なら、枠に載せていない人の全画面にする (dock_poll.h の focus_off_slot_)。
	focus_off_slot_ = !user_id.empty() &&
			  std::none_of(layout_state_.focus_tiles.begin(), layout_state_.focus_tiles.end(),
				       [&user_id](const sources::CompositeFocusTile &tile) { return tile.user_id == user_id; });
	push_layout_now(pipeline_.snapshot(), self_frame_shown_now());
}

void DockPoll::press_focus_hotkey(int cell)
{
	if (!timer_ || !settings_.feature_enabled)
		return;
	// 位置から人を選ぶのは、合成のソースに最後に渡した配置の入力 (全画面でないときの配置) で行う。
	const sources::FocusHotkeyTarget target =
		sources::focus_hotkey_target(layout_state_.focus_tiles, cell, focus_user_id_);
	if (!target.act)
		return;
	ledger::ActionArgs args;
	args.user_id = target.user_id;
	run_action(ledger::ActionId::SetFocus, args);
}

void DockPoll::press_focus_restore_hotkey()
{
	if (!timer_ || focus_user_id_.empty())
		return;
	run_action(ledger::ActionId::SetFocus, ledger::ActionArgs{});
}

void DockPoll::after_slot_release(config::SlotKind kind, int slot)
{
	// 割り当てが外れた画面共有の枠はすぐに空け、同じ処理の中で並べ直す (どうするかは sources/screen_end.h の
	// screen_end_action が決める)。名前ラベルに表示する人も外し、外した人の名前ラベルが空の枠に残らないように
	// する。声の枠は、映像を持たないので何もしない。
	if (kind != config::SlotKind::Screen)
		return;
	const sources::ScreenEndAction action = sources::screen_end_action(sources::ScreenEndEvent::RemovedFromSlot);
	if (action.clear_now)
		sources::clear_screen_now(slot);
	if (action.clear_owner)
		sources::set_decor_owner(slot, std::string());
	if (action.relayout)
		push_layout_now(pipeline_.snapshot(), self_frame_shown_now());
}

// ---- DockPollLayoutPart の実装 (dock_poll_layout.h) ----

void DockPollLayoutPart::attach(DockPoll &self)
{
	DockPollLayoutState &state = self.layout_state_;
	if (!state.attached) {
		state.attached = true;
		state.context = std::make_unique<QObject>();
		{
			std::lock_guard<std::mutex> lock(state.bridge->mu);
			state.bridge->context = state.context.get();
			state.bridge->owner = &self;
		}
		// 画面共有が終わって Pipeline が枠を空けた後に、並べ直す。受信スレッドから呼ばれるので、
		// 呼び出しを積むだけにする。
		std::shared_ptr<DockPollLayoutBridge> bridge = state.bridge;
		self.pipeline_.set_screen_layout_changed_sink([bridge] {
			post_to_ui(*bridge, bridge->relayout_requested, DockPollLayoutPart::run_requested_relayout);
		});
		// 録画ファイルの名前に使う語と書式は、ここではなく Pipeline が作られるときに ini から設定する
		// (pipeline.h の record_name_words)。ドックを作らない起動でも、最初の録画より前に設定するためである。
	}
	push_runtime_values(self);
}

void DockPollLayoutPart::detach(DockPoll &self)
{
	DockPollLayoutState &state = self.layout_state_;
	if (!state.attached)
		return;
	state.attached = false;
	// 外す順は、Pipeline のコールバック、宛先の順にする。前者は、外す関数から戻った時点で、実行中の呼び出しも
	// 終わっている。
	self.pipeline_.set_screen_layout_changed_sink(nullptr);
	{
		std::lock_guard<std::mutex> lock(state.bridge->mu);
		state.bridge->context = nullptr;
		state.bridge->owner = nullptr;
	}
	state.context.reset(); // まだ実行されていない呼び出しは Qt が捨てる
	state.bridge->relayout_requested = false;
}

void DockPollLayoutPart::on_settings_changed(DockPoll &self)
{
	push_runtime_values(self);
}

void DockPollLayoutPart::push_runtime_values(DockPoll &self)
{
	const config::Settings &settings = self.settings_;
	// 名前ラベルの見た目の値 (大きさ、色、フォント、話している人の光)。値が前と同じなら、sources は何もしない。
	sources::set_decor_label_params(sources::decor_label_params_from_settings(settings));
	// 合成のソースの名前ラベルと話している人の光の見た目の値と、画面共有していない参加者の並びの設定。
	sources::set_composite_label_look(sources::composite_label_look_from_settings(settings));
	sources::set_composite_roster_params(sources::composite_roster_params_from_settings(settings));
	// 枠の背景 (映像を描かない所の色と不透明度)。
	sources::set_composite_background(sources::composite_background_from_settings(settings));
	// ダミーの表示で動く線が、1 往復する時間。
	sources::set_dummy_cycle_ms(settings.dummy_cycle_ms);
	// 接続が途切れたときに、最後のフレームを表示し続ける時間。
	self.pipeline_.set_link_lost_hold_ms(settings.link_lost_hold_ms);
	// 接続し直してから、映像が戻らない枠の配置を保つ時間。
	self.pipeline_.set_reconnect_layout_hold_ms(settings.reconnect_layout_hold_ms);
	// 画面共有の映像の表示の緩衝。
	self.pipeline_.set_display_buffer_ms(settings.display_buffer_ms);
	// 録画ファイルの作成の進み具合が変わらないときに、止まったと判断するまでの秒数。
	self.pipeline_.set_finalize_stall_s(settings.record_finalize_stall_s);
}

void DockPollLayoutPart::run_requested_relayout(DockPoll &self)
{
	// 要求を false に戻してから並べ直す。並べ直しの間に来た次の要求は、もう 1 回積まれる。
	self.layout_state_.bridge->relayout_requested = false;
	self.push_layout_now(self.pipeline_.snapshot(), self.self_frame_shown_now());
}

void DockPollLayoutPart::push_composite_input(DockPoll &self, const pipeline::PipelineSnapshot &snapshot,
					      bool self_frame_shown)
{
	const config::Settings &settings = self.settings_;
	const std::vector<ledger::SeenPerson> &seen_people = self.seen_people_;
	const std::string self_owner = self_frame_user_id(settings, snapshot);

	// 見えている画面共有の枠は次の判断 (ダミーの間は今ある枠のすべて。そうでなければ割り当て表で出力して
	// よく、載っている人の画面共有のストリームが今ある枠) で、枠に載っている人の表示名 (ドックや名前ラベルと同じ
	// effective_display_name) と user_id を添える。並べ方は、設定の並び順で
	// sources/composite_layout.h の plan_composite_layout が決める。
	const bool dummy = sources::dummy_mode_active();
	std::vector<int> visible_slots;
	std::vector<sources::SlotPerson> people;
	if (dummy) {
		// ダミーのタイルの user_id は、全画面で映す人を見分けるためだけに使う (sources/dummy_feed.h の dummy_tile_user_id)。
		std::map<int, std::string> owners;
		for (const pipeline::SlotSnapshot &row : snapshot.slots) {
			if (row.kind == config::SlotKind::Screen && !row.owner_user_id.empty())
				owners.emplace(row.slot, row.owner_user_id);
		}
		for (int slot = 1; slot <= sources::kScreenSlotCount; ++slot) {
			sources::SlotPerson person;
			person.slot = slot;
			person.name = sources::dummy_display_name(slot);
			const auto owner = owners.find(slot);
			person.user_id = sources::dummy_tile_user_id(slot, owner != owners.end() ? owner->second : std::string());
			person.added_order = slot;
			people.push_back(std::move(person));
		}
	} else {
		visible_slots = pipeline::visible_screen_slots(snapshot.slots);
		for (const pipeline::SlotSnapshot &row : snapshot.slots) {
			if (row.kind != config::SlotKind::Screen || row.owner_user_id.empty())
				continue;
			sources::SlotPerson person;
			person.slot = row.slot;
			person.name = effective_display_name(seen_people, snapshot.profiles, row.owner_user_id);
			person.user_id = row.owner_user_id;
			person.added_order = row.added_order;
			person.is_main = !settings.broadcaster_user_id.empty() && row.owner_user_id == settings.broadcaster_user_id;
			people.push_back(std::move(person));
		}
	}
	// 1 つのときの全面は常に有効にする (設定の項目はない)。ダミー映像の間は、自分の枠の条件に依らず自分の枠を描かず、4 つの
	// ダミーを自分の枠がないときと同じ配置で描く (sources/composite_input.h の dummy_composite_input)。自分の枠のチェックと
	// 状態 (self_frame_.state) はダミーの間も変えないので、ダミーをやめると、自分の枠は前のとおりに描かれる。
	sources::CompositeInput input =
		dummy ? sources::dummy_composite_input(people, ledger::kFullWhenSingle, settings.tile_gap_px)
		      : sources::make_composite_input(visible_slots, people, self_frame_shown, ledger::kFullWhenSingle,
						      settings.tile_gap_px);
	input.self_hears_voice = self_frame_hears_voice(snapshot.slots, self_owner);
	input.slot_order = settings.slot_order;
	input.self_first = settings.self_slot_first;
	input.self_added_mark = self.self_frame_.mark.mark;
	// 自分の枠の人。メインアカウントだけの構成で Vesktop との接続が切れている間は、最後に分かった Vesktop のアカウントにする。
	// 自分の枠は接続が切れても描き続けるので、自分の枠の全画面も続ける。行の枠の位置 (Observation の Choices) と同じ関数で決める。
	const SelfFrameIdentity self_identity =
		self_frame_identity(settings.broadcaster_user_id, snapshot, seen_people, self.last_known_vesktop_user_id_);
	input.self_user_id = self_identity.user_id;
	input.self_name = self_identity.name;

	// 全画面。全画面にできるタイル (全画面でないときの配置) を決め直し、全画面の人を描かなくなっていればやめてから渡す。
	// Vesktop との接続が切れて映像を空けた人は、配置には残るが映っていないので、全画面にできるタイルから除く。
	std::vector<sources::CompositeFocusTile> focus_tiles = sources::composite_focus_tiles(input);
	std::vector<sources::CompositeFocusOther> focus_others;
	if (!dummy) {
		const std::set<std::string> emptied = pipeline::link_lost_emptied_owners(snapshot.slots);
		focus_tiles.erase(std::remove_if(focus_tiles.begin(), focus_tiles.end(),
						 [&emptied](const sources::CompositeFocusTile &tile) {
							 return emptied.count(tile.user_id) != 0;
						 }),
				  focus_tiles.end());
		// 配置に描いていないが、画面共有している人も全画面にできる (枠に載せていない人)。Vesktop のアカウントの画面共有は
		// 受信できず、自分の枠で出す人の画面は画面の枠に入れないので、除く。
		std::vector<std::string> excluded;
		if (snapshot.facts.self_user_id)
			excluded.push_back(*snapshot.facts.self_user_id);
		if (self.self_frame_selected() && !settings.broadcaster_user_id.empty())
			excluded.push_back(settings.broadcaster_user_id);
		const std::vector<std::string> sharing =
			snapshot.facts.sharing_valid ? snapshot.facts.sharing.user_ids : std::vector<std::string>();
		focus_others = sources::composite_focus_others(
			sharing, focus_tiles, excluded, [&](const std::string &user_id) {
				return effective_display_name(seen_people, snapshot.profiles, user_id);
			});
	}
	DockPollLayoutPart::settle_focus(self, std::move(focus_tiles), std::move(focus_others));
	input.focus_user_id = self.focus_user_id_;
	// 枠に載せていない人を全画面にしているときは、その人の映像を持つ子を渡す。画面の枠を持つ人 (描けずに dropped になった人) は
	// その枠の子、持たない人は全画面だけで映す人の子 (Pipeline::set_focus_target が経路を作る)。
	if (self.focus_off_slot_ && !self.focus_user_id_.empty()) {
		input.focus_extra_slot = config::kFocusOnlySlot;
		for (const pipeline::SlotSnapshot &row : snapshot.slots) {
			if (row.kind == config::SlotKind::Screen && row.owner_user_id == self.focus_user_id_)
				input.focus_extra_slot = row.slot;
		}
	}

	// 名前ラベルを出す人 (通話の参加者のうち、Bot、受け取りに使うアカウント、「通話のみ時ラベル非表示」を付けた人などを除いた
	// 全員。composite_roster_people)。合成のソースが、名前ラベル付きで描く枠がない人を、画面共有していない参加者の並びに描く
	// (枠に描く人は枠の名前ラベルに出る)。ダミーの間は渡さない (枠にはダミーの人が描かれ、通話の人の並びは見た目の確認に
	// 不要である)。
	std::vector<sources::CompositeRosterPerson> roster;
	// Vesktop との接続が切れて最後のフレームを残している間は、並びも切れる前のまま描き、映像と一緒に消す (切れると
	// facts の roster がなくなり、並びだけが先に消える)。
	const bool hold_roster =
		!dummy && !snapshot.facts.roster.has_value() && pipeline::link_lost_frame_held(snapshot.slots);
	if (hold_roster)
		roster = self.layout_state_.roster;
	else if (!dummy)
		roster = composite_roster_people(snapshot, settings, seen_people, self_owner);

	// 名前ラベルを出す人の名前とアバターを、合成のソースの並びの表に先に入れる (表にない人は並びに描かれない。いま枠に出ている
	// 人も入れておき、並びに移ったときにすぐ描けるようにする)。名前ラベルを出す人でなくなった人の値は消し、
	// 前の通話の人の名前とアバターを持ち続けない。
	std::set<std::string> roster_ids;
	std::map<std::string, std::vector<std::uint8_t>> roster_avatars;
	for (const sources::CompositeRosterPerson &person : roster) {
		sources::RosterPersonDecor decor;
		decor.name = person.name;
		const auto previous = self.layout_state_.roster_avatars.find(person.user_id);
		decor.avatar_png = decor_avatar(snapshot.profiles, person.user_id,
						previous != self.layout_state_.roster_avatars.end() ? &previous->second
												     : nullptr);
		roster_avatars[person.user_id] = decor.avatar_png;
		sources::set_roster_person_decor(person.user_id, decor);
		roster_ids.insert(person.user_id);
	}
	for (const std::string &user_id : self.layout_state_.roster_decor_people) {
		if (roster_ids.count(user_id) == 0)
			sources::forget_roster_person_decor(user_id);
	}
	self.layout_state_.roster_decor_people = std::move(roster_ids);
	self.layout_state_.roster_avatars = std::move(roster_avatars);
	self.layout_state_.roster = roster;

	input.roster = std::move(roster);
	sources::set_composite_input(input);
}

void DockPollLayoutPart::settle_focus(DockPoll &self, std::vector<sources::CompositeFocusTile> tiles,
				      std::vector<sources::CompositeFocusOther> others)
{
	const bool feature_on = self.settings_.feature_enabled;
	// 枠に載せていない人を全画面にしているときだけ、配置に描いていない人 (others) でも全画面を続ける。
	const std::string kept = sources::focus_after_layout(self.focus_user_id_, tiles, feature_on,
							     self.focus_off_slot_ ? others
										  : std::vector<sources::CompositeFocusOther>());
	if (kept != self.focus_user_id_) {
		diag::logf(LOG_INFO, "%s", feature_on ? "Full screen ended: that person is no longer shown"
						      : "Full screen ended: cordmux was disabled");
		self.focus_user_id_ = kept;
	}
	// 全画面の人が配置に描かれるようになったら (枠に載った)、枠に載っている人の全画面として扱う。その後に枠から外したら、
	// 全画面をやめる (外したのは、配信から消すためである)。
	if (self.focus_user_id_.empty() ||
	    std::any_of(tiles.begin(), tiles.end(),
			[&self](const sources::CompositeFocusTile &tile) { return tile.user_id == self.focus_user_id_; }))
		self.focus_off_slot_ = false;
	self.layout_state_.focus_tiles = std::move(tiles);
	self.layout_state_.focus_others = std::move(others);
	// 枠に載せていない人の全画面なら、その人を受信させる。それ以外は渡さない (枠に載っている人と自分の枠の人は、経路を
	// 変えない)。値が変わらなければ Pipeline は何もしない。
	self.pipeline_.set_focus_target(self.focus_off_slot_ ? self.focus_user_id_ : std::string());
}

void DockPollLayoutPart::sync_self_frame_source(DockPoll &self, const pipeline::PipelineSnapshot &snapshot)
{
	if (snapshot.frozen)
		return; // シーンコレクションの切り替えの途中は探さない (ソースが破棄される途中のため)

	// 指定があれば、見つかるまで周期ごとに探し直す。設定の uuid と名前が見つけたソースと違えば書き直す (名前で見つけたとき、
	// ソースの名前が変わったとき)。
	sources::SelfFrameSetting setting;
	setting.uuid = self.settings_.self_frame_source_uuid;
	setting.name = self.settings_.self_frame_source_name;
	const sources::SelfFrameSync sync = sources::sync_self_frame(setting);
	if (sync.rewrite) {
		self.settings_.self_frame_source_uuid = sync.uuid;
		self.settings_.self_frame_source_name = sync.name;
		self.write_settings();
	}
}

void DockPollLayoutPart::distribute_person_decor(DockPoll &self, const pipeline::PipelineSnapshot &snapshot)
{
	if (snapshot.frozen)
		return; // シーンコレクションの切り替えの途中は何も渡さない (合成のソースに渡す値と同じ)

	// 自分の枠に載せる人。サブアカウントを使う構成ではメインアカウント (broadcaster_user_id)、
	// メインアカウントだけの構成では Vesktop のアカウント (facts の self_user_id) にする (self_frame_user_id)。
	const std::string self_owner = self_frame_user_id(self.settings_, snapshot);
	sources::set_decor_owner(sources::kSelfSlot, self_owner);

	// 自分の枠の人の話している・話していないを決める音声ソース。指定があれば、コールバックの付け外しと探し直しを行い、
	// 設定の uuid と名前が見つけたソースと違えば書き直す (名前で見つけたとき、ソースの名前が変わったとき)。
	{
		sources::SelfSpeakingSetting setting;
		setting.uuid = self.settings_.self_speaking_source_uuid;
		setting.name = self.settings_.self_speaking_source_name;
		setting.threshold_db = self.settings_.self_speaking_threshold_db;
		const sources::SelfSpeakingSync sync = sources::sync_self_speaking(setting, self_owner);
		if (sync.rewrite) {
			self.settings_.self_speaking_source_uuid = sync.uuid;
			self.settings_.self_speaking_source_name = sync.name;
			self.write_settings();
		}
	}

	// 名前ラベルの値を渡す相手は、画面の枠に載っている人と、自分の枠に載せる人である。
	std::set<std::string> people;
	for (const pipeline::SlotSnapshot &row : snapshot.slots) {
		if (row.kind == config::SlotKind::Screen && !row.owner_user_id.empty())
			people.insert(row.owner_user_id);
	}
	if (!self_owner.empty())
		people.insert(self_owner);
	// 枠に載せていない人を全画面にしているときは、その人の名前ラベルも描く (全画面だけで映す人の子に描く)。
	if (self.focus_off_slot_ && !self.focus_user_id_.empty())
		people.insert(self.focus_user_id_);

	// 同じシーンに表示されている人。名前ラベルは、同じシーンで見分けのつかない名前を、真ん中を省いたりアバターを
	// 残したりして見分けられるようにするので、その判断に使う。映像を表示している画面の枠に載っている人と、
	// 表示していれば自分の枠に載せる人を入れる。同じ名前の人が 2 人いれば、その名前は 2 つ入る。
	// 映像を表示しているかは video_allowed で判断する。Vesktop との接続が切れて最後のフレームを残している人も、映って
	// いるので入れる (切れる前と同じ名前の描き方を保つ)。
	std::vector<std::pair<std::string, std::string>> visible; // (userId, 描画する名前)
	for (const pipeline::SlotSnapshot &row : snapshot.slots) {
		const bool frame_shown = row.share_present || (row.held_after_link_lost && !row.held_frame_cleared);
		if (row.kind == config::SlotKind::Screen && row.video_allowed && frame_shown && !row.owner_user_id.empty())
			visible.emplace_back(row.owner_user_id,
					     effective_display_name(self.seen_people_, snapshot.profiles, row.owner_user_id));
	}
	if (!self_owner.empty() && self.self_frame_shown_now())
		visible.emplace_back(self_owner, effective_display_name(self.seen_people_, snapshot.profiles, self_owner));

	std::map<std::string, std::vector<std::uint8_t>> avatars;
	for (const std::string &user_id : people) {
		sources::PersonDecor decor;
		// 描画するのは取得できた名前だけで、取得できなければ空にする (userId は描画しない)。
		decor.name = effective_display_name(self.seen_people_, snapshot.profiles, user_id);
		const auto previous = self.layout_state_.decor_avatars.find(user_id);
		decor.avatar_png = decor_avatar(snapshot.profiles, user_id,
						previous != self.layout_state_.decor_avatars.end() ? &previous->second : nullptr);
		avatars[user_id] = decor.avatar_png;
		// 名前ラベルの隅は全員一律の設定 (参加者ごとの値は持たない)。
		decor.corner = sources::decor_corner_of(self.settings_.label_corner);
		bool skipped_self = false;
		for (const auto &[other_id, other_name] : visible) {
			if (!skipped_self && other_id == user_id) {
				skipped_self = true; // 本人の名前を 1 つだけ除く
				continue;
			}
			if (!other_name.empty())
				decor.other_visible_names.push_back(other_name);
		}
		sources::set_person_decor(user_id, decor);
	}

	// いなくなった人の値を削除する。前の通話の人の名前とアバターを持ち続けない。
	for (const std::string &user_id : self.layout_state_.decor_people) {
		if (people.count(user_id) == 0)
			sources::forget_person_decor(user_id);
	}
	self.layout_state_.decor_people = std::move(people);
	self.layout_state_.decor_avatars = std::move(avatars);
}

} // namespace cordmux::dock
