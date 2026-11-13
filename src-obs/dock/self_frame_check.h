// SPDX-License-Identifier: Apache-2.0

// 自分の行のチェック (自分の枠を出すか) のうち、ドックが Pipeline に渡し、Pipeline の答えを受ける所の判断。DockPoll の
// push_assign_policy と carry_self_frame_check は、Pipeline を呼ぶ前後にここの関数を呼び、返った SelfFrameStep のとおりに、
// 持っている値 (DockPoll::self_frame_) を置き換え、ログを書き、settings.json を書き、空けた枠を片付ける
// (DockPoll::apply_self_frame_step)。Pipeline の答え (使い始めることを断った、枠を空けた、載せた) を受けた後の扱いを、
// 単体テストが製品の関数のまま確かめるために、DockPoll から分けてある。テストは Pipeline の代わりに、Pipeline が割り当て表と
// 同じロックの中で呼ぶ関数 (pipeline/stream_router.h の accepted_self_frame_in_use、pipeline/pipeline.h の move_self_frame_in) を
// 間に挟む (tests/test_dock_self_frame_check.cpp)。
// Qt も libobs も含まない。ログの行の文字列は DockPoll が持ち、ここは何を書くかだけを返す (ログレベルの定数が libobs にあるため)。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config/settings.h"
#include "ledger/observation.h"
#include "pipeline/pipeline.h"
#include "sources/self_frame_plan.h"

namespace cordmux::dock {

// layout_step が出した自分の枠の結果。配置の入力 (DockPoll::push_layout_now) と、tick が Observation に入れる値
// (dock/build_observation.h の DockFacts の同じ名前の欄) に使う。
struct SelfFrameFacts {
	bool self_frame_bound = false;
	bool self_frame_on = false;
	bool self_frame_shown = false;
	ledger::SelfShareState self_frame_share = ledger::SelfShareState::Unknown;
};

// ドックが持つ自分の枠の値 (DockPoll::self_frame_)。
struct SelfFrameCheck {
	// 自分の枠を出すかの状態 (sources/self_frame_plan.h。on は自分の行のチェックそのもの)。起動時の on は settings の
	// self_frame_on。外したか (excluded) と、直前の自分の共有 (prev) は残さない。
	sources::SelfFrameState state;
	// 最後に Pipeline が受け付けた、自分の枠を使っているか (Pipeline::set_assign_policy と move_self_frame の戻り値)。
	bool pushed_in_use = false;
	// 自分の枠を載せた順の印 (ledger/observation.h の Choices::self_frame_added_mark)。チェックが付いている間に、自分の枠が
	// 初めて実際に映った時に、その時の割り当て表の画面共有の枠の載せた順の番号の最大を入れて決める
	// (sources/self_frame_plan.h の next_self_order_mark。DockPoll::update_self_added_mark)。チェックが外れたら決め直す。映像を
	// 「Discordの画面共有」から OBS のソースに替えてチェックを引き継ぐときは、メインアカウントの枠の番号から決める
	// (plan_self_frame_carry)。
	sources::SelfOrderMark mark;
	// layout_step が出した結果。Pipeline が断ったときは、この周期の配置の入力からも自分の枠を外すので、ここも書き換える
	// (入れたままだと、その周期だけ 5 つになる)。
	SelfFrameFacts facts;
};

// DockPoll が OBS のログに書く行。文字列は dock/dock_poll.cpp の log_self_frame_line にある。
enum class SelfFrameLog {
	TurnedOnVideoChanged,  // 映像を OBS のソースに替え、メインアカウントのチェックを自分の枠に引き継いだ
	TurnedOffVideoChanged, // 映像を「Discordの画面共有」に替え、自分の枠のチェックをメインアカウントの枠に引き継いだ
	TurnedOffRefused,      // 自分の枠を使い始めることを Pipeline が断り、チェックを外した
	KeptNotSharing,        // 「Discordの画面共有」に替えたが、メインアカウントが画面共有していないので、チェックを残した
	KeptNoFreeSlot,        // 「Discordの画面共有」に替えたが、空いている枠がなかったので、チェックを残した
	RemovedFromSlot,       // メインアカウントの枠を空けた (slot はその枠)
	NotInSlot,             // メインアカウントの枠を空けようとしたが、もう枠に載っていなかった
};

struct SelfFrameLogLine {
	SelfFrameLog what = SelfFrameLog::TurnedOffRefused;
	int slot = 0; // RemovedFromSlot の枠の番号。ほかは 0
};

// 1 つの判断の結果。DockPoll は next で持っている値を置き換え、settings の self_frame_on を settings_on にし、logs を順に
// 書き、write_settings なら settings.json を書き、released_slot があれば空けた枠を片付ける (DockPoll::after_slot_release)。
struct SelfFrameStep {
	SelfFrameCheck next;
	bool settings_on = false;
	// settings.json を今書くか。Pipeline が断ってチェックを外したときだけ真にする。映像を替えたときの引き継ぎは、設定を
	// 替えた後処理 (DockPoll::on_settings_changed、on_look_changed) が書くので、ここでは書かない。
	bool write_settings = false;
	int released_slot = 0; // 空けた画面の枠の番号。0 はなし
	std::vector<SelfFrameLogLine> logs;
};

// 自分の枠を使っているか (config::self_frame_in_use。映像を OBS から選んでいて、自分の行のチェックが付いている。映像が
// 結び付いているかは見ない)。selected は、映像を OBS から選んでいるか (設定の値)。Pipeline に渡し、画面の枠に新しく載せられる
// 人数を 1 つ減らす。
bool self_frame_in_use_of(const SelfFrameCheck &check, bool selected);

// 自分の枠を使っているかが、最後に Pipeline が受け付けた値から変わったか (変わっていたら、DockPoll が渡し直す)。
bool self_frame_push_due(const SelfFrameCheck &check, bool selected);

// Pipeline が、自分の枠を使い始めることを断った (Discord の人がもう画面共有の枠を 4 つとも埋めていた)。付ける判断は
// スナップショット (最大 1 周期前) の人数で行うので、その後に自動で載せる経路が 4 人目を載せていると断られる。外し方は、
// 付けるはずの枠が埋まっていて見送ったときと同じで、excluded にはしない (sources/self_frame_plan.h の SelfFrameEvent::Refused。
// 自動なら枠が空けば付く)。settings.json にも残し、この周期の配置の入力 (facts) からも自分の枠を外す。settings_on は、今の
// settings の self_frame_on。
SelfFrameStep refuse_self_frame_step(const SelfFrameCheck &check, bool settings_on);

// Pipeline::set_assign_policy に、自分の枠を使っているかとして requested を渡し、accepted が返った後 (DockPoll::push_assign_policy)。
// 受け付けた値を覚え、真を渡して偽が返ったら、Pipeline が断ったので、チェックを外す (refuse_self_frame_step)。
SelfFrameStep assign_policy_pushed(const SelfFrameCheck &check, bool settings_on, bool requested, bool accepted);

// ---- 映す映像を替えたときのチェックの引き継ぎ (DockPoll::carry_self_frame_check) ----
//
// 自分の枠に映す映像を「Discordの画面共有」と OBS のソースかシーンの間で替えたときに、メインアカウントのチェックを引き継ぐ。
// 起動の途中の結び付きでは呼ばない。引き継ぐ場所 (メインアカウントの画面共有の枠と自分の枠) の付け替えは、Pipeline の 1 回の
// ロックの中で行う (Pipeline::move_self_frame)。分けると、その間に自動で待っていた人が空いた枠に載り、自分の枠を使い始め
// られなくなるか、メインアカウントを載せ直せなくなる。DockPoll は plan_self_frame_carry の before を当て、move が None でなければ
// Pipeline::move_self_frame を呼んで、答えを settle_self_frame_carry に渡す。

// 引き継ぐために読む、スナップショットと設定の事実。
struct SelfFrameCarryFacts {
	// 自分の枠の人 (dock/build_observation.h の self_frame_user_id)。
	std::string owner;
	// 引き継ぐものがあるか (自分の枠の人が分かり、サブアカウントがある)。サブアカウントがない構成 (自分の枠の人が Vesktop の
	// アカウント) では、その人の Discord の画面共有は受信側に届かず、Pipeline も画面の枠に載せない
	// (pipeline/stream_router.h の AssignPolicy::receiver_user_id) ので、引き継ぐものがない。
	bool carries = false;
	// owner が載っている画面共有の枠と、その載せた順の番号 (carries のときだけ。載っていなければ 0)。
	int owner_slot = 0;
	std::int64_t owner_order = 0;
	// owner が画面共有していないと分かっているか (facts の sharing を読めていて、その一覧にいない)。分からなければ偽。
	bool owner_not_sharing = false;
};

SelfFrameCarryFacts self_frame_carry_facts(const config::Settings &settings, const pipeline::PipelineSnapshot &snapshot);

// Pipeline に頼む付け替え。
enum class SelfFrameMoveKind {
	None,          // 頼まない
	ToSelfFrame,   // メインアカウントの枠を空けて、自分の枠を使い始める (Pipeline::move_self_frame の to_self_frame が真)
	ToScreenShare, // 自分の枠を使うのをやめて、メインアカウントを画面共有の枠に載せる (偽)
};

struct SelfFrameCarryPlan {
	SelfFrameMoveKind move = SelfFrameMoveKind::None;
	std::string owner;                       // move_self_frame に渡す人
	std::optional<std::int64_t> after_order; // ToScreenShare で、載せた順をこの番号の人の後にする (自分の枠の位置を引き継ぐ)
	// Pipeline を呼ぶ前に当てる値とログ。move が None なら、これで終わる。
	SelfFrameStep before;
};

// 引き継ぎ方を決める。now_selected は替えた後の値 (映像を OBS から選んでいるか)。settings_on は今の settings の self_frame_on。
//   OBS のソースかシーンに替えた: メインアカウントが画面共有の枠に載っていれば、そのチェックを自分の枠のチェックに移す
//     (ToSelfFrame。載せた順の位置は、メインアカウントの枠の位置のまま引き継ぐ)。載っていなければ、前から付いていたチェック
//     (「Discordの画面共有」か「(なし)」に替えたときに、メインアカウントを枠に載せられずに残したチェック) が、そのまま自分の枠の
//     チェックになる (None)。そのとき Discord の人が枠を 4 つとも埋めていたら、この後に DockPoll が渡す
//     push_assign_policy で Pipeline が断り、チェックを外す (assign_policy_pushed。ドックで付けられないのと同じ規則)。
//   「Discordの画面共有」に替えた: 自分の枠のチェックが付いていれば、メインアカウントを画面共有の枠に載せる (ToScreenShare。
//     載せた順は、自分の枠の印の人の後。印をまだ決めていなければ、ほかの人と同じく今の最後)。メインアカウントが画面共有して
//     いないと分かっていれば、チェックを残す (画面共有していない人は枠に載せられない)。
SelfFrameCarryPlan plan_self_frame_carry(const SelfFrameCheck &check, bool settings_on, const SelfFrameCarryFacts &facts,
					 bool now_selected);

// Pipeline::move_self_frame の答え (moved) を受けた後。plan の before を当てた後の値から決める。
//   ToSelfFrame: 受け付けた self_frame_in_use を覚え、空けた枠を片付ける。空けた枠の分で使うので普通は断られないが、メイン
//     アカウントの枠がスナップショットの後に空いて、自動で待っていた人が載っていれば断られ、チェックを外す
//     (refuse_self_frame_step)。
//   ToScreenShare: メインアカウントが載れば (もう載っていたときも)、自分の枠のチェックを外す。載らなければ (空いている枠が
//     なかった) チェックを残す。
SelfFrameStep settle_self_frame_carry(const SelfFrameCarryPlan &plan, const pipeline::Pipeline::SelfFrameMove &moved);

} // namespace cordmux::dock
