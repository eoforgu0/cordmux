// SPDX-License-Identifier: Apache-2.0

// テストの中でドックの周期の処理を行う SimPoll。製品の DockPoll (dock/dock_poll.cpp) の tick() と run_action() のうち、
// ドックのウィジェットに届く処理だけを同じ順で行う (Observation、ledger::decide_dock、ledger::stabilize、
// DockWidget::render、set_busy_actions、スクリーンリーダーへの通知)。DockPoll は libobs と Pipeline と
// OS の問い合わせに依存するので、そのままは組み込めない。Observation はテストデータから作って自分で持ち、
// 押された操作は製品と同じく ledger::apply() に通してから、結果を Observation に反映する。判断はすべて製品の
// ledger が行い、View を手で作らない。

#pragma once

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "config/allocation.h"
#include "config/settings.h"
#include "dock/deferred_save.h"
#include "dock/ui/dock_style.h"
#include "fixtures.h"
#include "ledger/actions.h"
#include "ledger/dock_view.h"
#include "ledger/notice_tracker.h"
#include "ledger/observation.h"
#include "ledger/status_tab.h"
#include "pipeline/pipeline.h"
#include "sources/composite_input.h"
#include "sources/self_frame_plan.h"

class OBSDock;

namespace cordmux::dock {
class AudioTab;
class DockWidget;
class PeopleList;
} // namespace cordmux::dock

namespace cordmux::dock_drive {

// 押された操作 1 つの記録。
struct ActionRecord {
	std::int64_t t_ms = 0; // テストの開始からの ms (単調増加の時計)
	ledger::ActionId id = ledger::ActionId::SetSlot;
	ledger::ActionArgs args;
	ledger::IntentKind kind = ledger::IntentKind::None;
	bool ran = false; // ledger::apply が押せると答えた
	bool from_rename = false; // 「名前を変更...」から来た (ダイアログは表示しない)
};

// Observation の時刻は、テストデータの時刻 (tests/ui_preview/fixtures.cpp の kNow) に実時間の経過を加えたものである
// (テストデータの中の「何秒前」がそのまま意味を持つ)。周期は製品と同じ 200 ms (DockPollConfig::poll_ms の既定値)。
//
// DockPoll から再現した部分。DockPoll を変えたら、ここも確認する。
//   - tick の順と、set_busy_actions に渡す値 (DockPoll::tick)
//   - 1 行目を ledger::settle_first_line で決め直すこと、確認に使う値を渡すこと、タブを替えたら設定に残すこと
//   - 枠に載せた人の視聴の範囲に入れた時刻 (Pipeline の watching_since_us。apply_intent)
//   - 操作の後に、0 ms で周期の処理を 1 回行うこと (DockPoll::run_action の最後)
//   - スクリーンリーダーへの通知。決め方は再現せず、製品の dock::announcements_for を呼ぶ。再現するのは呼ぶ順と、
//     一覧が前から表示されていたかのフラグ (DockPoll::tick、report_notice_changes、rendered_people_)
//   - OBS のログの行の数 (メッセージの表示と消去の 1 つに 1 行。DockPoll::report_notice_changes)。行は書かず、
//     数だけを数える
//   - マイクの光と音声タブのメーターの値を読む周期 (DockPoll::level_timer_。enable_level_timer で始める)
//   - 状態タブの累計と履歴と最後の録画 (DockPoll::tick の ledger::settle_status と、リセットの ledger::reset_status)。
//     直近 1 分の列 (obs().receive.arrival) は、状態タブが見えている間だけ Observation に入れる (arrival() にテストが置く)
//   - 映像のバッファ (状態タブの入力欄。DockPoll::on_buffer_changed)。表示タブと同じ保存の待ちを使う
//   - 表示タブで変えた値を設定に入れ、保存を 1 回にまとめること (DockPoll::on_look_changed と look_save_。同じ
//     dock::DeferredSave と dock::apply_look_tab_fields を使う)。配信の画面へ渡す呼び出し (Pipeline と名前ラベル) は
//     libobs に依存するので、on_look_reflect に置き換える
class SimPoll {
public:
	SimPoll(const ui_preview::Fixture &base, const dock::ui::DockStyle &style);
	~SimPoll();
	SimPoll(const SimPoll &) = delete;
	SimPoll &operator=(const SimPoll &) = delete;

	// 周期の処理を始める。製品と同じく、最初の 1 回はすぐに行う。
	void start(int poll_ms = 200);
	void stop();
	// 周期の処理を 1 回行う (DockPoll::tick と同じ順)。周期の処理の中から呼ばれたときは何もしない。
	void tick();

	// ---- ウィジェット ----
	OBSDock *dock() const { return dock_.get(); }
	dock::DockWidget *widget() const { return widget_; }
	dock::PeopleList *people() const; // なければ nullptr
	dock::AudioTab *audio() const;    // 音声タブの中身。なければ nullptr

	// ---- 現在の値 ----
	ledger::Observation &obs() { return obs_; }
	const ledger::Observation &obs() const { return obs_; }
	config::Settings &settings() { return settings_; }
	const ledger::dockview::DockViewV2 &view() const { return view_; }
	const ledger::dockview::DockViewV2 &raw() const { return raw_; }
	std::int64_t now_us() const;
	std::int64_t elapsed_ms() const { return clock_.elapsed(); }
	int ticks() const { return ticks_; }
	// Observation を書き換えた回数。テストが書き換えるたびに 1 増える (時計で変わるフィールドは数えない)。
	std::uint64_t model_version() const { return model_version_; }
	void changed() { ++model_version_; }

	// ---- テストが書き換える Observation の値 ----
	// フレームを止める。枠に載っていて画面共有している人の、最後のフレームの時刻を、周期ごとに現在の時刻へ進めるのを
	// やめる。
	void set_frames_stopped(const std::string &user_id, bool stopped);
	bool frames_stopped(const std::string &user_id) const { return stopped_frames_.count(user_id) != 0; }
	// 処理中の操作 (DockPoll の install_state_ の代わり)。次の周期で set_busy_actions に渡す。
	void set_busy(const std::set<ledger::ActionId> &busy);
	// 0 ms の後に周期の処理を 1 回行う (操作の後と同じ)。周期を始めていなければ、すぐに行う。
	void tick_soon();
	// 周期ごとに、自分の Discord の共有 (obs().choices.self_frame_share) の始まりと終わりで自分の枠を点け消しする
	// (DockPoll::step_self_frame と同じく sources::plan_self_frame に通す)。既定では行わず、自分の枠を出すかは
	// テストが書いた値のままになる。押された SetSelfFrame は、この設定によらず plan_self_frame を通して反映する。
	void follow_self_frame_share(bool on) { poll_self_frame_ = on; }
	// 共有の始まりで点けようとして、枠が埋まっていたので見送った回数 (DockPoll が出すログの行の数に当たる)。
	int self_frame_deferred() const { return self_frame_deferred_; }
	// 「名前を変更...」で製品のダイアログ (dock/rename_dialog.h) を表示するようにする。既定では on_rename を呼び、
	// ダイアログは表示しない。
	void use_product_rename_dialog();

	// ---- 音の大きさ (DockPoll::level_timer_ の再現) ----
	// マイクの光と音声タブのメーターの値を読む周期を始める。製品と同じく、ドックが見せたいと伝えている間 (DockWidget::
	// set_on_levels_wanted) だけ、100 ms ごとに levels() を読んで DockWidget に渡す。DockWidget の set_on_levels_wanted を
	// 使うので、同じ SimPoll で自分のコールバックを置くシナリオは呼ばない。
	void enable_level_timer();
	// 周期が動いているか。
	bool level_timer_running() const { return level_timer_ != nullptr && level_timer_->isActive(); }
	// 周期が読んだ回数。
	int level_reads() const { return level_reads_; }
	// 周期が読む値 (Pipeline::audio_levels の代わり)。
	void set_levels(pipeline::AudioLevels levels) { levels_ = std::move(levels); }
	const pipeline::AudioLevels &levels() const { return levels_; }

	// ---- 表示タブ (DockPoll::on_look_changed と look_save_ の再現) ----
	// 表示タブで値を変えて、配信の画面へ渡した回数と、settings.json に保存した回数。
	int look_reflects() const { return look_reflects_; }
	int look_saves() const { return look_saves_; }
	// 最後に保存した設定 (製品は settings.json に書く)。
	const config::Settings &saved_settings() const { return saved_settings_; }
	// 渡す呼び出しと保存の順 (テストの開始からの ms と、'r' が渡した、's' が保存した)。
	struct LookEvent {
		std::int64_t t_ms = 0;
		char kind = 'r';
	};
	const std::vector<LookEvent> &look_events() const { return look_events_; }
	// 保存を待っているか。
	bool look_save_pending() const { return look_save_ != nullptr && look_save_->pending(); }
	// 表示タブで変えたまま渡していない値と、待っている保存を、今行う (DockPoll::stop)。
	void flush_look();
	// 表示タブの値を設定に入れた後 (保存の前) に呼ぶ。製品では Pipeline と名前ラベルへ渡す呼び出しに当たる。
	std::function<void(const config::Settings &)> on_look_reflect;

	// ---- 状態タブ ----
	// 直近 1 分の列の元 (Pipeline の列の代わり)。状態タブが見えている間だけ obs().receive.arrival に入れる。
	std::vector<ledger::ArrivalFact> &arrival() { return arrival_; }
	const ledger::StatusState &status_state() const { return status_state_; }
	// 状態タブで映像のバッファを変えて、配信の処理へ渡した回数。
	int buffer_reflects() const { return buffer_reflects_; }

	// ---- コールバック ----
	// 操作が押されたとき (記録した後、Observation に反映する前) に呼ぶ。true を返したときは、反映をシナリオが
	// 済ませたとみなし、SimPoll は反映しない。
	std::function<bool(const ActionRecord &)> on_action;
	// 周期の処理の後 (描いた後) に呼ぶ。
	std::function<void(const ledger::StabilizeResult &)> on_tick;
	// 「名前を変更...」が選ばれたときに呼ぶ (ダイアログは表示しない)。
	std::function<void(const std::string &user_id)> on_rename;

	// 押されたすべての操作の記録。
	const std::vector<ActionRecord> &actions() const { return actions_; }
	// OBS のログに書かれるはずの行の数 (メッセージの表示と消去の 1 つに 1 行。DockPoll::report_notice_changes)。
	std::int64_t notice_log_lines() const { return notice_log_lines_; }

	// 操作を実行する (ウィジェットのコールバックから来たときと同じ処理)。ledger::apply() が押せると答えたときだけ、
	// 結果を Observation に反映する。
	void run_action(ledger::ActionId id, const ledger::ActionArgs &args, bool from_rename = false);

private:
	// 画面共有をやめた人と通話から抜けた人の枠を空ける。製品は facts が届くたびに行う (pipeline/slot_release.h)。
	void release_departed();
	// 全画面にできる人と全画面の人の名前を決め直し、全画面の人を描かなくなっていれば全画面をやめる。製品は合成のソースに
	// 配置の入力を渡すたびに行う (dock/dock_poll_layout.cpp の push_composite_input と settle_focus)。ここでは
	// Observation から、製品と同じ人を同じ名前で配置の入力に入れる (ダミー映像の表示の間の全画面は再現しない)。
	void settle_focus();
	void apply_intent(const ledger::Intent &intent);
	void on_look_changed(const config::Settings &from_tab);
	void on_buffer_changed(int ms);
	// 自分の枠を出すかの状態を、event で 1 つ進めて Observation と設定に反映する (DockPoll::step_self_frame と
	// DockPoll::set_self_frame を合わせた処理)。
	void step_self_frame(sources::SelfFrameEvent event);
	void announce(const ledger::NoticeDiff &diff, const ledger::dockview::FirstLine &first_line);

	std::unique_ptr<OBSDock> dock_;
	dock::DockWidget *widget_ = nullptr;
	ledger::Observation obs_;
	config::Settings settings_;
	std::int64_t base_now_us_ = 0;
	QElapsedTimer clock_;
	std::unique_ptr<QTimer> timer_;
	std::unique_ptr<QTimer> level_timer_;
	std::unique_ptr<dock::DeferredSave> look_save_;
	int look_reflects_ = 0;
	int look_saves_ = 0;
	config::Settings saved_settings_;
	std::vector<LookEvent> look_events_;
	pipeline::AudioLevels levels_;
	int level_reads_ = 0;
	ledger::NoticeTrackerState tracker_;
	ledger::NoticeReadState read_state_;
	ledger::StatusState status_state_;
	std::vector<ledger::ArrivalFact> arrival_;
	int buffer_reflects_ = 0;
	ledger::dockview::DockViewV2 view_;
	ledger::dockview::DockViewV2 raw_;
	std::set<ledger::ActionId> busy_;
	std::set<std::string> stopped_frames_;
	std::vector<ActionRecord> actions_;
	std::int64_t notice_log_lines_ = 0;
	std::uint64_t model_version_ = 0;
	int ticks_ = 0;
	bool in_tick_ = false;
	sources::SelfFrameState self_frame_; // on は Observation の値を使う。外したかと直前の共有だけをここに持つ
	bool poll_self_frame_ = false;
	int self_frame_deferred_ = 0;
	bool rendered_people_ = false; // 前の周期に一覧を描いたか (DockPoll::rendered_people_ と同じ)
};

} // namespace cordmux::dock_drive
