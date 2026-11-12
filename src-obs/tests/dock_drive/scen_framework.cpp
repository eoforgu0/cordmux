// SPDX-License-Identifier: Apache-2.0

// framework のシナリオ。ドックの枠組み (1 行目、メインとサブの行、タブ、下のバー) と参加者タブの動きを確認する。
//   (1) メッセージが出たときと消えたときに、1 行目、メインとサブの行、タブ、下のバー、行の座標が動かない。
//   (2) 1 行目を押すと状態タブが開き、レベル 2 のメッセージが既読になる。
//   (3) タブを切り替えると中身が替わり、設定 dock_tab に残る。ホイールではタブが替わらない。
//   (4) 押した場所の隣の確認 ([退出]。配信中だけ) の、やめる、確定、5 秒で戻る、続けて押しても確定しない。
//   (5) 枠が埋まっているときのチェックは、チェックを変えず、入れ替える相手のメニューを開く。
//   (6) 行が組の間を移る変化は、ポインターが一覧の上で動いている間は保留し、止まって layout_hold_idle_ms がたったら反映する。
//   (7) マイクの光を読む周期は、参加者タブが見えている間は動かし、状態タブでは止める。光は、話している人のマイクのボタンだけが
//       持つ (音声タブは audio のシナリオが確認する)。

#include "scenarios.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QGuiApplication>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyleHints>
#include <QTabBar>
#include <QWidget>

#include <map>
#include <string>
#include <vector>

#include "dock/dock_widget.h"
#include "dock/first_line.h"
#include "dock/inline_confirm.h"
#include "dock/people_list.h"
#include "dock/person_row.h"
#include "dock/ui/text_resolve.h"
#include "ledger/text_ids.h"
#include "drive_env.h"
#include "input.h"
#include "obs_dock.h"
#include "query.h"
#include "scen_common.h"
#include "sim_poll.h"

namespace cordmux::dock_drive {

namespace {

namespace keys = ledger::text::keys;

const QString kGroup = QStringLiteral("framework");

// 動かしてはならない要素の矩形 (スクリーン座標)。
std::map<QString, QRect> fixed_rects(dock::DockWidget *w)
{
	std::map<QString, QRect> out;
	for (const char *name : {"cordmuxFirstLine", "cordmuxAccounts", "cordmuxAccountChange", "cordmuxAccountMove",
				 "cordmuxAccountLeave", "cordmuxTabs", "cordmuxPeopleHeader", "cordmuxRecordButton",
				 "cordmuxDockMenuButton"}) {
		QWidget *part = w->findChild<QWidget *>(QString::fromLatin1(name));
		if (part != nullptr)
			out[QString::fromLatin1(name)] = global_rect(part);
	}
	return out;
}

std::map<std::string, QRect> row_rects(SimPoll &sim)
{
	std::map<std::string, QRect> out;
	if (sim.people() == nullptr)
		return out;
	for (const std::string &id : sim.people()->shown_user_ids()) {
		if (dock::PersonRowWidget *row = sim.people()->row_for(id))
			out[id] = global_rect(row);
	}
	return out;
}

QStringList moved_parts(const std::map<QString, QRect> &before, const std::map<QString, QRect> &now)
{
	QStringList moved;
	for (const auto &[name, rect] : before) {
		const auto found = now.find(name);
		if (found == now.end() || found->second != rect)
			moved << name;
	}
	return moved;
}

int moved_rows(const std::map<std::string, QRect> &before, const std::map<std::string, QRect> &now)
{
	int moved = 0;
	for (const auto &[id, rect] : before) {
		const auto found = now.find(id);
		if (found == now.end() || found->second != rect)
			++moved;
	}
	return moved;
}

QPushButton *button_named(dock::DockWidget *w, const char *name)
{
	return w->findChild<QPushButton *>(QString::fromLatin1(name));
}

// ポインターでウィジェットの中央を押す。動かしてからガードの時間より長く待つ。
void click_after_wait(Input &input, QWidget *w, int wait_ms = 700)
{
	input.move_to(w->mapToGlobal(w->rect().center()));
	pump(wait_ms);
	input.click();
	pump(100);
}

int ran_count(const SimPoll &sim, ledger::ActionId id)
{
	int n = 0;
	for (const ActionRecord &a : sim.actions()) {
		if (a.id == id && a.ran)
			++n;
	}
	return n;
}

// サブアカウントが通話に参加していて、[退出] を押せる場面にする。
void set_sub_in_call(SimPoll &sim)
{
	ledger::Observation &obs = sim.obs();
	obs.choices.broadcaster_user_id = "1000";
	obs.call.presence = ledger::Presence::InCall;
	obs.call.name = "general";
	sim.changed();
}

} // namespace

void run_framework(const ScenarioOptions &options, Results &results)
{
	// ---- (1) メッセージが出たときと消えたときに、何も動かない ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-4"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 700);
		sim.dock()->show();
		sim.start();
		pump(600);
		const std::map<QString, QRect> fixed = fixed_rects(sim.widget());
		const std::map<std::string, QRect> rows = row_rects(sim);
		results.expect_true(kGroup, QStringLiteral("(1) 前提: 行がある"), !rows.empty());
		// レベル 1 (空き容量が尽きて録画が止まった) とレベル 2 (バージョンの食い違い) を出す。
		sim.obs().recording.stopped_by_space_exhausted = true;
		sim.obs().versions.discord_side = "0.0.9";
		sim.changed();
		pump(2800);
		const bool shown = sim.view().first_line.tier == std::optional<ledger::dockview::Tier>(ledger::dockview::Tier::L1);
		results.expect_true(kGroup, QStringLiteral("(1) 前提: レベル 1 が 1 行目に出た"), shown);
		QStringList moved = moved_parts(fixed, fixed_rects(sim.widget()));
		results.expect_true(kGroup, QStringLiteral("(1) メッセージが出ても、1 行目、メインとサブの行、タブ、下のバーが動かない"), moved.isEmpty(),
				    moved.join(QLatin1Char(',')));
		results.expect_eq(kGroup, QStringLiteral("(1) メッセージが出たときに動いた行の数"), moved_rows(rows, row_rects(sim)), 0);
		// 同じ場面で、1 行目だけが危険の色になる。
		QWidget *first = sim.widget()->findChild<QWidget *>(QStringLiteral("cordmuxFirstLine"));
		results.expect_true(kGroup, QStringLiteral("(1) レベル 1 の間は、1 行目に危険の色の背景がある"),
				    first != nullptr && first->property("l1Surface").toBool());
		sim.obs().recording.stopped_by_space_exhausted = false;
		sim.obs().versions.discord_side = "0.1.0+dev";
		sim.changed();
		pump(2000);
		moved = moved_parts(fixed, fixed_rects(sim.widget()));
		results.expect_true(kGroup, QStringLiteral("(1) メッセージが消えても、1 行目、メインとサブの行、タブ、下のバーが動かない"), moved.isEmpty(),
				    moved.join(QLatin1Char(',')));
		results.expect_eq(kGroup, QStringLiteral("(1) メッセージが消えたときに動いた行の数"), moved_rows(rows, row_rects(sim)), 0);
		sim.stop();
		sim.dock()->hide();
	}

	// ---- (2) 1 行目を押すと状態タブが開き、レベル 2 が既読になる ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-4"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 700);
		sim.dock()->show();
		Input input;
		input.activate(sim.dock());
		sim.start();
		pump(600);
		sim.obs().versions.discord_side = "0.0.9"; // VersionRestartVesktop (レベル 2)
		sim.changed();
		pump(2800);
		results.expect_true(kGroup, QStringLiteral("(2) 前提: 未読のレベル 2 が 1 行目に出ている"),
				    sim.view().first_line.kind == ledger::dockview::FirstLineKind::Notice);
		QWidget *first = sim.widget()->findChild<QWidget *>(QStringLiteral("cordmuxFirstLine"));
		click_after_wait(input, first);
		pump(100);
		results.expect_true(kGroup, QStringLiteral("(2) 1 行目を押すと状態タブが開く"),
				    sim.widget()->tab() == config::DockTab::Status);
		results.expect_true(kGroup, QStringLiteral("(2) 状態タブを開くと、レベル 2 は 1 行目から消える"),
				    sim.view().first_line.kind != ledger::dockview::FirstLineKind::Notice);
		results.expect_true(kGroup, QStringLiteral("(2) 状態タブにメッセージのカードがある"),
				    !shown_notice_keys(sim.widget()).isEmpty(),
				    shown_notice_keys(sim.widget()).join(QLatin1Char(',')));
		auto *tabs = sim.widget()->findChild<QTabBar *>(QStringLiteral("cordmuxTabs"));
		results.expect_true(kGroup, QStringLiteral("(2) 状態タブに件数が付く"),
				    tabs != nullptr && tabs->tabText(3).contains(QLatin1Char('1')),
				    tabs != nullptr ? tabs->tabText(3) : QString());
		sim.stop();
		sim.dock()->hide();
	}

	// ---- (3) タブの切り替え ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 700);
		sim.dock()->show();
		Input input;
		input.activate(sim.dock());
		sim.start();
		pump(600);
		auto *tabs = sim.widget()->findChild<QTabBar *>(QStringLiteral("cordmuxTabs"));
		results.expect_true(kGroup, QStringLiteral("(3) 前提: タブが 4 つある"), tabs != nullptr && tabs->count() == 4);
		if (tabs != nullptr && tabs->count() == 4) {
			// タブの幅は同じ (1 px の丸めの差まで)。
			int min_w = tabs->tabRect(0).width();
			int max_w = min_w;
			for (int i = 1; i < 4; ++i) {
				min_w = std::min(min_w, tabs->tabRect(i).width());
				max_w = std::max(max_w, tabs->tabRect(i).width());
			}
			results.expect_true(kGroup, QStringLiteral("(3) タブの幅が同じ"), max_w - min_w <= 1,
					    QStringLiteral("%1 から %2 px").arg(min_w).arg(max_w));
			const QRect rect = tabs->tabRect(2);
			click_after_wait(input, tabs, 0);
			input.move_to(tabs->mapToGlobal(rect.center()));
			pump(100);
			input.click();
			pump(100);
			results.expect_true(kGroup, QStringLiteral("(3) タブを押すと切り替わり、設定に残る"),
					    sim.widget()->tab() == config::DockTab::Display &&
						    sim.settings().dock_tab == config::DockTab::Display);
			input.wheel(-120);
			pump(100);
			results.expect_true(kGroup, QStringLiteral("(3) タブのバーの上のホイールでは切り替わらない"),
					    sim.widget()->tab() == config::DockTab::Display);
			auto *focus = QApplication::focusWidget();
			results.expect_true(kGroup, QStringLiteral("(3) タブを押してもタブのバーにフォーカスが移らない"), focus != tabs);
		}
		sim.stop();
		sim.dock()->hide();
	}

	// ---- (4) 押した場所の隣の確認 ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-4"), options.style);
		set_sub_in_call(sim);
		sim.dock()->setGeometry(1300, 120, 420, 700);
		sim.dock()->show();
		Input input;
		input.activate(sim.dock());
		sim.start();
		pump(600);
		// [退出] の確認は、配信中だけに出る。
		sim.obs().uninstall.obs_streaming = true;
		sim.changed();
		pump(700);
		const std::map<QString, QRect> fixed = fixed_rects(sim.widget());
		auto *confirm = dynamic_cast<dock::InlineConfirm *>(sim.widget()->findChild<QWidget *>(QStringLiteral("cordmuxInlineConfirm")));
		QPushButton *leave = button_named(sim.widget(), "cordmuxAccountLeave");
		results.expect_true(kGroup, QStringLiteral("(4) 前提: [退出] を押せる"),
				    leave != nullptr && leave->isVisible() && leave->isEnabled());
		if (confirm != nullptr && leave != nullptr && leave->isEnabled()) {
			const QRect leave_rect = global_rect(leave);
			click_after_wait(input, leave);
			results.expect_true(kGroup, QStringLiteral("(4) [退出] を押すと確認が出る"), confirm->active());
			results.expect_eq(kGroup, QStringLiteral("(4) 確認の間は [退出] の操作を実行していない"),
					  ran_count(sim, ledger::ActionId::LeaveCall), 0);
			results.expect_true(kGroup, QStringLiteral("(4) ポインターの下は やめる側のボタンである"),
					    global_rect(confirm->cancel_button()).contains(leave_rect.center()));
			QStringList moved = moved_parts({{QStringLiteral("cordmuxFirstLine"), fixed.at(QStringLiteral("cordmuxFirstLine"))},
							 {QStringLiteral("cordmuxTabs"), fixed.at(QStringLiteral("cordmuxTabs"))},
							 {QStringLiteral("cordmuxRecordButton"), fixed.at(QStringLiteral("cordmuxRecordButton"))}},
							fixed_rects(sim.widget()));
			results.expect_true(kGroup, QStringLiteral("(4) 確認が出ても、ほかの要素は動かない"), moved.isEmpty(), moved.join(QLatin1Char(',')));
			// 続けて押しても確定しない (同じ位置で、ガードの時間の内にもう一度押す)。
			input.click();
			pump(50);
			results.expect_eq(kGroup, QStringLiteral("(4) 続けて押しても確定しない"), ran_count(sim, ledger::ActionId::LeaveCall), 0);
			results.expect_true(kGroup, QStringLiteral("(4) 続けて押しても確認は残る"), confirm->active());
			results.expect_true(kGroup, QStringLiteral("(4) やめる側のボタンの文言は、押した結果が分かる [通話を続ける]"),
					    confirm->cancel_button()->text() == dock::ui::resolve_key(keys::kDockConfirmStayInCallButton),
					    confirm->cancel_button()->text());
			// やめる側のボタン ([通話を続ける]) で戻る。
			pump(700);
			input.click();
			pump(100);
			results.expect_true(kGroup, QStringLiteral("(4) [通話を続ける] で元に戻る"), !confirm->active() && leave->isVisible());
			results.expect_eq(kGroup, QStringLiteral("(4) [通話を続ける] では実行しない"), ran_count(sim, ledger::ActionId::LeaveCall), 0);
			// 確定。
			pump(600);
			input.click();
			pump(700);
			QPushButton *yes = confirm->confirm_button();
			input.move_to(yes->mapToGlobal(yes->rect().center()));
			pump(50);
			input.click();
			pump(100);
			results.expect_eq(kGroup, QStringLiteral("(4) [確定] で 1 回実行する"), ran_count(sim, ledger::ActionId::LeaveCall), 1);
			results.expect_true(kGroup, QStringLiteral("(4) [確定] の後は元に戻る"), !confirm->active());
			// 5 秒で戻る。ポインターはボタンから離しておく。
			sim.obs().call.presence = ledger::Presence::InCall;
			sim.changed();
			pump(600);
			click_after_wait(input, leave);
			results.expect_true(kGroup, QStringLiteral("(4) もう一度 [退出] を押すと確認が出る"), confirm->active());
			input.move_to(QPoint(900, 500));
			pump(dock::kInlineConfirmTimeoutMs + 300);
			results.expect_true(kGroup, QStringLiteral("(4) 操作がなければ 5 秒で元に戻る"), !confirm->active() && leave->isVisible());
			results.expect_eq(kGroup, QStringLiteral("(4) 5 秒で戻るときは実行しない"), ran_count(sim, ledger::ActionId::LeaveCall), 1);
			// 配信していないときは、確認せずにすぐ実行する。
			sim.obs().uninstall.obs_streaming = false;
			sim.obs().call.presence = ledger::Presence::InCall;
			sim.changed();
			pump(700);
			click_after_wait(input, leave);
			results.expect_true(kGroup, QStringLiteral("(4) 配信していないときは、[退出] を確認せずにすぐ実行する"),
					    !confirm->active() && ran_count(sim, ledger::ActionId::LeaveCall) == 2);
		}
		// 録画の終了は、配信中でも確認しない (押したらすぐ終える)。
		sim.obs().uninstall.obs_streaming = true;
		sim.obs().choices.recording = ledger::FeatureSwitch::On;
		sim.obs().recording.phase = ledger::RecordPhase::Recording;
		sim.changed();
		pump(700);
		QPushButton *record = button_named(sim.widget(), "cordmuxRecordButton");
		if (confirm != nullptr && record != nullptr) {
			const std::size_t before = sim.actions().size();
			click_after_wait(input, record);
			results.expect_true(kGroup, QStringLiteral("(4) 配信中に録画を終えるときも確認は出ず、すぐ実行する"),
					    !confirm->active() && sim.actions().size() > before);
		}
		sim.stop();
		sim.dock()->hide();
	}

	// ---- (5) 枠が埋まっているときの入れ替え ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 700);
		sim.dock()->show();
		Input input;
		input.activate(sim.dock());
		sim.start();
		pump(600);
		const ledger::dockview::PersonRow *target = nullptr;
		for (const ledger::dockview::PersonRow &row : sim.view().people_list.sharing) {
			if (row.check_replaces)
				target = &row;
		}
		for (const ledger::dockview::PersonRow &row : sim.view().people_list.in_call) {
			if (target == nullptr && row.check_replaces)
				target = &row;
		}
		results.expect_true(kGroup, QStringLiteral("(5) 前提: 入れ替えのチェックの行がある"), target != nullptr);
		if (target != nullptr) {
			const std::string user = target->user_id;
			const std::string replaced = target->replace_choices.front().action.args.replace_user_id;
			dock::PersonRowWidget *row = sim.people()->row_for(user);
			const bool checked_before = row->check()->isChecked();
			const QRect hit = check_hit_rect_global(sim.people(), user);
			input.move_to(hit.center());
			pump(700);
			int items = 0;
			bool seen_menu = false;
			bool check_kept = false;
			// メニューは入れ子のイベントループで開くので、開いている間に確認して、項目を選ぶ。
			QTimer::singleShot(200, [&] {
				auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
				seen_menu = menu != nullptr && menu->objectName() == QLatin1String("cordmuxReplaceMenu");
				check_kept = row->check()->isChecked() == checked_before;
				if (menu != nullptr) {
					for (QAction *action : menu->actions()) {
						if (action->property("userId").toString().toStdString() == replaced) {
							++items;
							action->trigger();
						}
					}
				}
			});
			input.click();
			pump(500);
			results.expect_true(kGroup, QStringLiteral("(5) 薄いチェックを押すと、入れ替える相手のメニューが開く"), seen_menu);
			results.expect_true(kGroup, QStringLiteral("(5) メニューを開いてもチェックは変わらない"), check_kept);
			results.expect_eq(kGroup, QStringLiteral("(5) 選ぶと 1 回の ReplaceSlot で入れ替わる"), ran_count(sim, ledger::ActionId::ReplaceSlot), items > 0 ? 1 : -1);
			results.expect_eq(kGroup, QStringLiteral("(5) SetSlot は実行しない (途中の並びが配信に出ない)"), ran_count(sim, ledger::ActionId::SetSlot), 0);
			bool now_slotted = false;
			for (const ledger::PersonFact &p : sim.obs().people) {
				if (p.user_id == user)
					now_slotted = p.slot.has_value();
			}
			results.expect_true(kGroup, QStringLiteral("(5) 選んだ後、押した人が枠に載っている"), now_slotted);
		}
		sim.stop();
		sim.dock()->hide();
	}

	// ---- (5b) 画面共有していない人のチェックは押せない ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-4"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 700);
		sim.dock()->show();
		sim.start();
		pump(600);
		// 枠に載っていなくて画面共有していない人 (画面共有していると分かっている人のうち、していない人) を探す。
		std::string idle;
		for (const ledger::dockview::PersonRow &row : sim.view().people_list.in_call) {
			if (!row.checked && !row.sharing && !row.self_frame_check && row.checkable && idle.empty())
				idle = row.user_id;
		}
		results.expect_true(kGroup, QStringLiteral("(5b) 前提: 枠に載っていなくて画面共有していない人がいる"), !idle.empty());
		if (!idle.empty()) {
			dock::PersonRowWidget *row = sim.people()->row_for(idle);
			results.expect_true(kGroup, QStringLiteral("(5b) 画面共有していない人のチェックは押せない (薄く表示する)"),
					    row != nullptr && row->check() != nullptr && !row->check()->isEnabled());
			results.expect_true(kGroup,
					    QStringLiteral("(5b) ツールチップは「画面共有していません。画面共有を始めると載せられます。」(row_check_not_sharing.tip)"),
					    row != nullptr && row->check() != nullptr &&
						    row->check()->toolTip().contains(dock::ui::resolve_key(keys::kDockRowCheckNotSharingTip)),
					    row != nullptr && row->check() != nullptr ? row->check()->toolTip() : QString());
			// 画面共有を始めると、チェックが押せる。
			for (ledger::PersonFact &p : sim.obs().people) {
				if (p.user_id == idle)
					p.sharing = true;
			}
			sim.changed();
			pump(700);
			row = sim.people()->row_for(idle);
			results.expect_true(kGroup, QStringLiteral("(5b) 画面共有を始めると、チェックが押せる"),
					    row != nullptr && row->check() != nullptr && row->check()->isEnabled());
		}
		sim.stop();
		sim.dock()->hide();
	}

	// ---- (6) 組の間の移動の保留と反映 ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-4"), options.style);
		sim.settings().layout_hold_idle_ms = 1000;
		sim.dock()->setGeometry(1300, 120, 420, 700);
		sim.dock()->show();
		Input input;
		input.activate(sim.dock());
		sim.start();
		pump(600);
		// 共有を始める人: 通話中の組の先頭の人。いなければ、画面共有中の組の先頭の人に共有をやめさせる。
		std::string mover;
		bool to_sharing = false;
		if (!sim.view().people_list.in_call.empty()) {
			mover = sim.view().people_list.in_call.front().user_id;
			to_sharing = true;
		} else if (!sim.view().people_list.sharing.empty()) {
			mover = sim.view().people_list.sharing.front().user_id;
		}
		results.expect_true(kGroup, QStringLiteral("(6) 前提: 組の間を移す人がいる"), !mover.empty());
		const auto in_group_widgets = [&sim](const std::string &user, QLabel *heading) {
			dock::PersonRowWidget *row = sim.people() != nullptr ? sim.people()->row_for(user) : nullptr;
			return row != nullptr && heading != nullptr ? row->y() > heading->y() : false;
		};
		auto *in_call_heading = sim.widget()->findChild<QLabel *>(QStringLiteral("cordmuxGroupInCall"));
		const bool below_before = in_group_widgets(mover, in_call_heading);
		auto *scroll = sim.widget()->findChild<QScrollArea *>(QStringLiteral("cordmuxScroll"));
		const QRect vp = scroll != nullptr ? global_rect(scroll->viewport()) : QRect();
		// ポインターを一覧の上で動かす。
		input.move_to(QPoint(vp.center().x(), vp.top() + 30));
		pump(50);
		results.expect_true(kGroup, QStringLiteral("(6) ポインターが一覧の上で動いている間は保留する"), sim.widget()->layout_held());
		for (ledger::PersonFact &p : sim.obs().people) {
			if (p.user_id == mover) {
				p.sharing = to_sharing;
				p.sharing_known = true;
			}
		}
		sim.changed();
		// 動かし続ける間は反映しない。
		for (int i = 0; i < 6; ++i) {
			pump(250);
			input.move_to(QPoint(vp.center().x() + (i % 2 == 0 ? 3 : -3), vp.top() + 30));
		}
		pump(50);
		const bool below_held = in_group_widgets(mover, in_call_heading);
		results.expect_true(kGroup, QStringLiteral("(6) 動いている間は、行が組の間を移らない"), below_held == below_before);
		// 止まって時間がたったら反映する。
		pump(1500);
		results.expect_true(kGroup, QStringLiteral("(6) 止まって layout_hold_idle_ms がたったら保留が解ける"), !sim.widget()->layout_held());
		const bool below_after = in_group_widgets(mover, in_call_heading);
		results.expect_true(kGroup, QStringLiteral("(6) 止まった後に、行が組の間を移る"), below_after != below_before);
		// 動いた直後の行は、ダブルクリックの時間だけクリックを受け付けない (既存の規則)。
		dock::PersonRowWidget *row = sim.people()->row_for(mover);
		results.expect_true(kGroup, QStringLiteral("(6) 前提: 動いた行がある"), row != nullptr);
		// ポインターが一覧の外なら保留しない。
		input.move_to(QPoint(900, 500));
		pump(50);
		results.expect_true(kGroup, QStringLiteral("(6) ポインターが一覧の外なら保留しない"), !sim.widget()->layout_held());
		sim.stop();
		sim.dock()->hide();
	}

	// ---- (7) マイクの光 ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-audio"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 700);
		std::vector<bool> reported;
		sim.widget()->set_on_levels_wanted([&reported](bool wanted) { reported.push_back(wanted); });
		sim.dock()->show();
		sim.start();
		pump(400);
		results.expect_true(kGroup, QStringLiteral("(7) 参加者タブが見えている間は、周期を動かす"), sim.widget()->levels_wanted());
		std::string talking;
		std::string silent;
		for (const ledger::dockview::PersonRow &row : sim.view().people_list.sharing) {
			if (row.mic == ledger::dockview::MicState::Normal && talking.empty())
				talking = row.user_id;
			else if (row.mic == ledger::dockview::MicState::Normal)
				silent = row.user_id;
		}
		for (const ledger::dockview::PersonRow &row : sim.view().people_list.in_call) {
			if (row.mic == ledger::dockview::MicState::Normal && silent.empty())
				silent = row.user_id;
		}
		results.expect_true(kGroup, QStringLiteral("(7) 前提: マイクのボタンを押せる行が 2 つある"), !talking.empty() && !silent.empty());
		if (!talking.empty() && !silent.empty()) {
			sim.widget()->set_speaking({talking});
			results.expect_true(kGroup, QStringLiteral("(7) 話している人のボタンだけが光る"),
					    sim.people()->row_for(talking)->voice_mute()->speaking() &&
						    !sim.people()->row_for(silent)->voice_mute()->speaking());
		}
		sim.widget()->set_tab(config::DockTab::Status);
		pump(50);
		results.expect_true(kGroup, QStringLiteral("(7) ほかのタブを開くと、周期を止める"), !sim.widget()->levels_wanted());
		if (!talking.empty())
			results.expect_true(kGroup, QStringLiteral("(7) 周期を止めたら光も消す"),
					    !sim.people()->row_for(talking)->voice_mute()->speaking());
		sim.widget()->set_tab(config::DockTab::People);
		pump(50);
		results.expect_true(kGroup, QStringLiteral("(7) 参加者タブへ戻ると、周期を動かす"), sim.widget()->levels_wanted());
		sim.dock()->hide();
		pump(50);
		results.expect_true(kGroup, QStringLiteral("(7) ドックが見えなくなると、周期を止める"), !sim.widget()->levels_wanted());
		results.expect_true(kGroup, QStringLiteral("(7) 動かすか止めるかをコールバックで伝えた"), reported.size() >= 3);
		sim.widget()->set_on_levels_wanted(nullptr);
		sim.stop();
	}
}

} // namespace cordmux::dock_drive
