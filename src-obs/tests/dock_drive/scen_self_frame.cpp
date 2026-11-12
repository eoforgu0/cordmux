// SPDX-License-Identifier: Apache-2.0

// selfframe のシナリオ。自分の行のチェックで自分の枠を付け外しすること、自分の Discord の共有の始まりで、ほかの人と同じ
// 規則 (載せる人の選び方が自動) でだけ自分の枠が付くことを、ポインターで押して確認する。テストデータ people-self-frame-off /
// -full / -on / -solo / -solo-no-video を使う。
//   (a) 自分の行のチェックを付ける・外す (操作が通り、次の周期の行に出て、付け外しした値が設定に残る)。
//   (b) Discord の人が枠を 4 つとも埋めているときは付けられない。外す向きは、埋まっていても押せる。
//   (c) 自分の共有の始まりは、載せる人の選び方が自動のときだけ自分の枠を付ける。終わりでは外れない。枠が埋まっていれば
//       見送る。
//   (d) ドックで外した後は、共有をやめて始め直すまで付かない。ドックで付けた分は共有の始まりと終わりで変わらない。
//   (e) 共有の状態が分からない間は変わらない。
//   (f) メインアカウントだけの構成の自分の行 (Vesktop のアカウント) にチェックと、[自動で載せる] だけのメニューが出る。
//   (g) 自分の枠に映す映像を OBS から選んでいない間は、サブアカウントがあれば行のチェックがほかの人と同じ (枠に載せる)。
//       サブアカウントがなければ、チェックは押せず、理由が付く。
//   (h) チェックが付いていて、選んだ映像が見つからない間は、行の状態がそれを伝える。見つかると消える。

#include "scenarios.h"

#include <QWidget>

#include <algorithm>
#include <string>

#include "dock/dock_widget.h"
#include "dock/people_list.h"
#include "dock/person_row.h"
#include "dock/ui/text_resolve.h"
#include "drive_env.h"
#include "input.h"
#include "ledger/text_ids.h"
#include "obs_dock.h"
#include "query.h"
#include "scen_common.h"
#include "sim_poll.h"

namespace cordmux::dock_drive {

namespace {

namespace keys = ledger::text::keys;

const ledger::dockview::PersonRow *row_named(const SimPoll &sim, const QString &name)
{
	for (const ledger::dockview::PersonRow &row : sim.view().people) {
		if (dock::ui::resolve_label(row.name) == name)
			return &row;
	}
	return nullptr;
}

dock::PersonRowWidget *widget_named(SimPoll &sim, const QString &name)
{
	const ledger::dockview::PersonRow *r = row_named(sim, name);
	return r != nullptr ? sim.people()->row_for(r->user_id) : nullptr;
}

// 押せる状態のチェックをポインターで押し、次の周期が終わるまで待つ。
void click_check(SimPoll &sim, Input &input, const QString &name)
{
	const ledger::dockview::PersonRow *row = row_named(sim, name);
	if (row == nullptr)
		return;
	// 行が現れた直後と動いた直後のクリックは捨てられる (ダブルクリックの時間)。
	pump(700);
	const QRect hit = check_hit_rect_global(sim.people(), row->user_id);
	if (!hit.isValid())
		return;
	input.move_to(hit.center());
	input.click();
	pump(700);
}

bool checked_of(SimPoll &sim, const QString &name)
{
	const ledger::dockview::PersonRow *r = row_named(sim, name);
	dock::PersonRowWidget *w = widget_named(sim, name);
	return r != nullptr && r->checked && w != nullptr && w->check() != nullptr && w->check()->isChecked();
}

int count_of(const SimPoll &sim, ledger::ActionId id)
{
	return static_cast<int>(std::count_if(sim.actions().begin(), sim.actions().end(),
					      [&](const ActionRecord &r) { return r.id == id && r.ran; }));
}

// 場面を開いて始める。
void open(SimPoll &sim, Input &input)
{
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	input.activate(sim.dock());
	sim.start();
	pump(1500);
}

void close(SimPoll &sim)
{
	sim.stop();
	sim.dock()->hide();
}

// 自分の共有の値を替えて、次の周期が終わるまで待つ。
void set_share(SimPoll &sim, ledger::SelfShareState share)
{
	sim.obs().choices.self_frame_share = share;
	sim.changed();
	pump(1200);
}

} // namespace

void run_self_frame(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("self-frame");
	const QString self = QStringLiteral("Taro Yamada");

	// ---- (a) 付ける・外す ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-self-frame-off"), options.style);
		Input input;
		open(sim, input);
		const ledger::dockview::PersonRow *row = row_named(sim, self);
		results.expect_true(group, QStringLiteral("(a) 自分の行にチェックがあり、外れていて、押せる"),
				    row != nullptr && row->self_frame_check && !row->checked && row->check_enabled &&
					    widget_named(sim, self) != nullptr && widget_named(sim, self)->check() != nullptr);
		const ledger::dockview::PersonRow *other = row_named(sim, QStringLiteral("hanako"));
		results.expect_true(group, QStringLiteral("(a) ほかの人の行のチェックは今までの意味 (枠に載せる)"),
				    other != nullptr && !other->self_frame_check &&
					    other->check_action.id == ledger::ActionId::SetSlot);
		click_check(sim, input, self);
		results.expect_eq(group, QStringLiteral("(a) チェックを付けると SetSelfFrame が 1 回通る"),
				  count_of(sim, ledger::ActionId::SetSelfFrame), 1);
		results.expect_true(group, QStringLiteral("(a) 付けると自分の枠が出て、行のチェックが付く"),
				    sim.obs().choices.self_frame_on && sim.obs().choices.self_frame_shown &&
					    checked_of(sim, self));
		results.expect_true(group, QStringLiteral("(a) 付けた値が設定に残る"), sim.settings().self_frame_on);
		click_check(sim, input, self);
		results.expect_eq(group, QStringLiteral("(a) チェックを外すと SetSelfFrame がもう 1 回通る"),
				  count_of(sim, ledger::ActionId::SetSelfFrame), 2);
		results.expect_true(group, QStringLiteral("(a) 外すと自分の枠が消えて、行のチェックが外れる"),
				    !sim.obs().choices.self_frame_on && !sim.obs().choices.self_frame_shown &&
					    !checked_of(sim, self));
		results.expect_true(group, QStringLiteral("(a) 外した値が設定に残る"), !sim.settings().self_frame_on);
		results.expect_eq(group, QStringLiteral("(a) 自分の行を押しても、枠の割り当ては動かない"),
				  count_of(sim, ledger::ActionId::SetSlot), 0);
		close(sim);
	}

	// ---- (b) 枠が 4 つとも埋まっている ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-self-frame-full"), options.style);
		Input input;
		open(sim, input);
		const ledger::dockview::PersonRow *row = row_named(sim, self);
		const bool reason_ok = row != nullptr && row->check_action.disabled_reason.has_value() &&
				       row->check_action.disabled_reason->id == keys::kDockNoFreeSlotText;
		results.expect_true(group, QStringLiteral("(b) Discord の人が 4 人いると、自分の行のチェックは押せず、理由が枠の空きがないこと"),
				    row != nullptr && row->self_frame_check && !row->check_enabled && reason_ok);
		sim.run_action(ledger::ActionId::SetSelfFrame, [&] {
			ledger::ActionArgs a;
			a.user_id = row != nullptr ? row->user_id : std::string();
			a.on = true;
			return a;
		}());
		pump(700);
		results.expect_true(group, QStringLiteral("(b) 押せない操作は何も変えない"),
				    !sim.actions().empty() && !sim.actions().back().ran && !sim.obs().choices.self_frame_on &&
					    !sim.settings().self_frame_on);
		// 自分の枠を出している状態なら、4 人いても外せる (消す向きは枠の埋まりによらない)。
		sim.obs().choices.self_frame_on = true;
		sim.obs().choices.self_frame_shown = true;
		sim.changed();
		pump(1200);
		const ledger::dockview::PersonRow *on_row = row_named(sim, self);
		results.expect_true(group, QStringLiteral("(b) 自分の枠を出している間は、4 人いても外す向きは押せる"),
				    on_row != nullptr && on_row->checked && on_row->check_enabled);
		close(sim);
	}

	// ---- (c) 共有の始まり: ほかの人と同じ規則でだけ付く ----
	{
		// 手動で [自動で載せる] なしなら、共有を始めても付かない。
		SimPoll sim(ui_preview::fixture_by_id("people-self-frame-off"), options.style);
		Input input;
		sim.follow_self_frame_share(true);
		open(sim, input);
		results.expect_true(group, QStringLiteral("(c) 共有していない間は、自分の枠は出ず、チェックは外れている"),
				    !sim.obs().choices.self_frame_shown && !checked_of(sim, self));
		set_share(sim, ledger::SelfShareState::On);
		results.expect_true(group, QStringLiteral("(c) 手動のときは、共有を始めても自分の枠は出ず、チェックも付かない"),
				    !sim.obs().choices.self_frame_on && !sim.obs().choices.self_frame_shown &&
					    !checked_of(sim, self));
		results.expect_eq(group, QStringLiteral("(c) 共有の始まりでは、ドックの操作は通らない"),
				  count_of(sim, ledger::ActionId::SetSelfFrame), 0);
		close(sim);
	}
	{
		// 載せる人の選び方が自動なら、共有の始まりで付く。ほかの人と同じく、設定にも残る。共有の終わりでは外れない。
		SimPoll sim(ui_preview::fixture_by_id("people-self-frame-off"), options.style);
		Input input;
		sim.obs().choices.assign_mode = ledger::AssignMode::Auto;
		sim.follow_self_frame_share(true);
		open(sim, input);
		set_share(sim, ledger::SelfShareState::On);
		results.expect_true(group, QStringLiteral("(c) 自動のときは、共有を始めると自分の枠が出て、チェックが付く"),
				    sim.obs().choices.self_frame_on && sim.obs().choices.self_frame_shown && checked_of(sim, self));
		results.expect_true(group, QStringLiteral("(c) 付いた値は設定に残る"), sim.settings().self_frame_on);
		set_share(sim, ledger::SelfShareState::Off);
		results.expect_true(group, QStringLiteral("(c) 共有を終えても、チェックは付いたまま。自分の枠も出たまま"),
				    sim.obs().choices.self_frame_on && sim.obs().choices.self_frame_shown && checked_of(sim, self));
		close(sim);
	}
	{
		// 枠が埋まっているときは付けず、押しのけない。
		SimPoll sim(ui_preview::fixture_by_id("people-self-frame-full"), options.style);
		Input input;
		sim.obs().choices.assign_mode = ledger::AssignMode::Auto;
		sim.follow_self_frame_share(true);
		open(sim, input);
		set_share(sim, ledger::SelfShareState::On);
		results.expect_true(group, QStringLiteral("(c) 枠が 4 つとも埋まっている間の共有の始まりは、押しのけず見送る"),
				    !sim.obs().choices.self_frame_on && !checked_of(sim, self) && sim.self_frame_deferred() >= 1);
		close(sim);
	}

	// ---- (d) ドックで外した後は、共有をやめて始め直すまで付かない ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-self-frame-off"), options.style);
		Input input;
		sim.obs().choices.assign_mode = ledger::AssignMode::Auto;
		sim.follow_self_frame_share(true);
		open(sim, input);
		set_share(sim, ledger::SelfShareState::On);
		click_check(sim, input, self);
		pump(1200);
		results.expect_true(group, QStringLiteral("(d) 共有の間に外すと、外れたままで、チェックも外れている"),
				    !sim.obs().choices.self_frame_on && !checked_of(sim, self) &&
					    count_of(sim, ledger::ActionId::SetSelfFrame) == 1);
		set_share(sim, ledger::SelfShareState::Off);
		set_share(sim, ledger::SelfShareState::On);
		results.expect_true(group, QStringLiteral("(d) 共有をやめて始め直すと、また付く"),
				    sim.obs().choices.self_frame_on && sim.obs().choices.self_frame_shown && checked_of(sim, self));
		close(sim);
	}
	{
		// ドックで付けた分は、共有の始まりと終わりで変わらない。
		SimPoll sim(ui_preview::fixture_by_id("people-self-frame-off"), options.style);
		Input input;
		sim.follow_self_frame_share(true);
		open(sim, input);
		click_check(sim, input, self);
		set_share(sim, ledger::SelfShareState::On);
		set_share(sim, ledger::SelfShareState::Off);
		results.expect_true(group, QStringLiteral("(d) ドックで付けた自分の枠は、共有を終えても残り、チェックも付いたまま"),
				    sim.obs().choices.self_frame_on && sim.obs().choices.self_frame_shown && checked_of(sim, self) &&
					    sim.settings().self_frame_on);
		close(sim);
	}

	// ---- (e) 共有の状態が分からない間は変わらない ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-self-frame-off"), options.style);
		Input input;
		sim.obs().choices.assign_mode = ledger::AssignMode::Auto;
		sim.follow_self_frame_share(true);
		open(sim, input);
		set_share(sim, ledger::SelfShareState::Unknown);
		results.expect_true(group, QStringLiteral("(e) 分からない間は、消えている自分の枠を出さない"),
				    !sim.obs().choices.self_frame_on && !checked_of(sim, self));
		set_share(sim, ledger::SelfShareState::On);
		set_share(sim, ledger::SelfShareState::Unknown);
		results.expect_true(group, QStringLiteral("(e) 分からなくなっても、付いた自分の枠は消えない"),
				    sim.obs().choices.self_frame_on && sim.obs().choices.self_frame_shown && checked_of(sim, self));
		close(sim);
	}

	// ---- (f) メインアカウントだけの構成 (映像を OBS から選んでいる) ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-self-frame-solo"), options.style);
		Input input;
		open(sim, input);
		const QString account = QStringLiteral("cordmux-receiver");
		const ledger::dockview::PersonRow *row = row_named(sim, account);
		dock::PersonRowWidget *w = widget_named(sim, account);
		results.expect_true(group, QStringLiteral("(f) Vesktop のアカウントの行に、チェックと [...] が出る (ミュートは出ない)"),
				    row != nullptr && w != nullptr && !row->checkable && row->self_frame_check &&
					    w->check() != nullptr && w->dots()->isVisibleTo(w) &&
					    !w->voice_mute()->isVisibleTo(w) && !w->screen_mute()->isVisibleTo(w));
		results.expect_true(group, QStringLiteral("(f) メニューは [自動で載せる] だけ"),
				    row != nullptr && row->menu.auto_add_only &&
					    row->menu.always_on_toggle.id == ledger::ActionId::SetAlwaysOn);
		results.expect_true(group, QStringLiteral("(f) 自分の枠を出している間は、チェックが付いていて押せる"),
				    row != nullptr && row->checked && row->check_enabled && w != nullptr && w->check() != nullptr &&
					    w->check()->isChecked() && w->check()->isEnabled());
		click_check(sim, input, account);
		results.expect_true(group, QStringLiteral("(f) 押すと自分の枠が消えて、チェックが外れる"),
				    count_of(sim, ledger::ActionId::SetSelfFrame) == 1 && !sim.obs().choices.self_frame_on &&
					    !checked_of(sim, account));
		close(sim);
	}

	// ---- (g) 映像を OBS から選んでいない間 (「Discord の画面共有」) ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-self-frame-off"), options.style);
		Input input;
		open(sim, input);
		sim.obs().choices.self_frame_selected = false;
		sim.obs().choices.self_frame_bound = false;
		sim.changed();
		pump(1200);
		const ledger::dockview::PersonRow *row = row_named(sim, self);
		results.expect_true(group, QStringLiteral("(g) サブアカウントがあれば、メインアカウントの行はほかの人と同じ (枠に載せる)"),
				    row != nullptr && !row->self_frame_check && row->checkable &&
					    row->check_action.id == ledger::ActionId::SetSlot);
		click_check(sim, input, self);
		const ledger::PersonFact *fact = nullptr;
		for (const ledger::PersonFact &p : sim.obs().people) {
			if (p.is_self_frame)
				fact = &p;
		}
		results.expect_true(group, QStringLiteral("(g) 押すと、Discord の画面共有を枠に載せる (自分の枠は動かない)"),
				    count_of(sim, ledger::ActionId::SetSlot) == 1 &&
					    count_of(sim, ledger::ActionId::SetSelfFrame) == 0 && fact != nullptr &&
					    fact->slot.has_value() && !sim.obs().choices.self_frame_on);
		close(sim);
	}
	{
		SimPoll sim(ui_preview::fixture_by_id("people-self-frame-solo-no-video"), options.style);
		Input input;
		open(sim, input);
		const QString account = QStringLiteral("cordmux-receiver");
		const ledger::dockview::PersonRow *row = row_named(sim, account);
		dock::PersonRowWidget *w = widget_named(sim, account);
		const bool reason_ok = row != nullptr && row->check_action.disabled_reason.has_value() &&
				       row->check_action.disabled_reason->id == keys::kDockRowSelfNoVideoTip;
		results.expect_true(group, QStringLiteral("(g) サブアカウントがなく、映像を選んでいなければ、チェックは出るが押せず、理由が付く"),
				    row != nullptr && w != nullptr && row->self_frame_check && !row->check_enabled && reason_ok &&
					    w->check() != nullptr && !w->check()->isEnabled());
		results.expect_true(group, QStringLiteral("(g) その行にも [自動で載せる] だけのメニューが出る"),
				    row != nullptr && w != nullptr && row->menu.auto_add_only && w->dots()->isVisibleTo(w));
		close(sim);
	}

	// ---- (h) 選んだ映像が見つからない ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-self-frame-on"), options.style);
		Input input;
		open(sim, input);
		const auto shows_missing = [&] {
			const ledger::dockview::PersonRow *r = row_named(sim, self);
			return r != nullptr && r->status.has_value() && r->status->text.id == keys::kDockRowSelfVideoMissingStatus;
		};
		results.expect_true(group, QStringLiteral("(h) 映像が見つかっている間は、行の状態に出ない"), !shows_missing());
		sim.obs().choices.self_frame_bound = false;
		sim.obs().choices.self_frame_shown = false;
		sim.changed();
		pump(3500);
		results.expect_true(group, QStringLiteral("(h) 見つからない間は、チェックが付いたまま、行の状態に出る"),
				    shows_missing() && checked_of(sim, self));
		dock::PersonRowWidget *w = widget_named(sim, self);
		const auto *text = w != nullptr ? w->findChild<QWidget *>(QStringLiteral("cordmuxRowStatusText")) : nullptr;
		// 状態のテキストは幅で表示か非表示かが決まる。開いた直後のドックは、大きさが変わるまで幅の判定が前の値のままなので、
		// ほかの状態と同じ条件で見るために大きさを変えてから見る。
		sim.dock()->setGeometry(1300, 120, 430, 700);
		pump(800);
		results.expect_true(group, QStringLiteral("(h) 行の状態のテキストが画面に表示される"),
				    text != nullptr && text->isVisibleTo(w));
		sim.obs().choices.self_frame_bound = true;
		sim.obs().choices.self_frame_shown = true;
		sim.changed();
		pump(3500);
		results.expect_true(group, QStringLiteral("(h) 見つかると、行の状態から消える"), !shows_missing());
		close(sim);
	}
}

} // namespace cordmux::dock_drive
