// SPDX-License-Identifier: Apache-2.0

// positions のシナリオ。配信者にとっていちばん悪い条件で、操作する場所が動かないことと、はみ出さないことを確認する。
// 押し間違いは配信に直接出るので、メッセージや外部の出来事で、押す場所を動かさない。画面は開かない。
//   (a) ドックにメッセージを出すすべての場面で、1 行目、メインとサブの行、タブのバー、下のバーの座標と大きさが、メッセージが
//       ない同じ構成の場面と同じ。場面は ui_preview の場面の一覧から機械的に集める (新しい場面を足せば、自動で対象になる)。
//       参加者の一覧が同じ場面は、行の座標も同じ。
//   (b) 回線の弱い人の「映像途切れ」が 1 人に 20 回出たり消えたりする間、1 行目、タブ、行の座標が動かず、1 行目が
//       メッセージのレベル 1 の見た目にならず、状態タブの累計が 20 回になる。
//   (c) 30 人。参加者タブ、音声タブ、状態タブで、横にはみ出さず、スクロールで全員に届き、ホイールで値が変わらない。
//   (d) ドックの大きさを 235x300 (最小の幅より狭い) にしても、幅が最小の幅になり、一覧の高さが 0 にならず、タブと下のバーを
//       押せる。
//   (e) メッセージのレベル 1、2、3 が同時に複数あるとき、1 行目は 1 つだけで、状態タブに全部あり、座標が動かない。
//   (f) 「枠に載せた人だけ」で、10 分前から共有している 2 人のチェックを押すと、行が「映像待ち」になり、メッセージは
//       待ち時間までは出ず、最初の映像が届くと消える。座標は動かない。

#include "scenarios.h"

#include <QApplication>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabBar>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "config/settings.h"
#include "dock/audio_tab.h"
#include "dock/bottom_bar.h"
#include "dock/build_observation.h"
#include "dock/dock_tabs.h"
#include "dock/dock_widget.h"
#include "dock/first_line.h"
#include "dock/notice_list.h"
#include "dock/people_list.h"
#include "dock/person_row.h"
#include "dock/status_tab.h"
#include "dock/ui/slider_spin.h"
#include "dock/ui/volume_slider.h"
#include "drive_env.h"
#include "fixtures.h"
#include "input.h"
#include "ledger/dock_view.h"
#include "ledger/status_tab.h"
#include "obs_dock.h"
#include "query.h"
#include "scen_common.h"
#include "sim_poll.h"

namespace cordmux::dock_drive {

namespace {

namespace dv = ledger::dockview;
const QString kGroup = QStringLiteral("positions");

QPoint center_of(QWidget *w)
{
	return w->mapToGlobal(w->rect().center());
}

QScrollArea *scroll_of(SimPoll &sim, const char *name)
{
	return sim.widget()->findChild<QScrollArea *>(QString::fromLatin1(name));
}

const char *scroll_name_of(config::DockTab tab)
{
	switch (tab) {
	case config::DockTab::People:
		return "cordmuxScroll";
	case config::DockTab::Audio:
		return "cordmuxAudioScroll";
	case config::DockTab::Display:
		return "cordmuxDisplayScroll";
	case config::DockTab::Status:
		break;
	}
	return "cordmuxStatusScroll";
}

// 条件が成り立つまで、最大 timeout_ms の間、イベントを処理する。step ごとに each を呼ぶ。成り立ったら true を返す。
bool wait_until(const std::function<bool()> &cond, int timeout_ms, const std::function<void()> &each = nullptr)
{
	for (int waited = 0; waited <= timeout_ms; waited += 25) {
		if (cond())
			return true;
		pump(25);
		if (each)
			each();
	}
	return cond();
}

// 場面の Observation で映像が止まっている人 (枠に載っていて画面共有していて、フレームが届いていない、または古い) の
// フレームを、SimPoll が周期ごとに進めないようにする。SimPoll は枠に載っている画面共有の人のフレームを届け続けるので、
// 止まっている場面は止めておかないと、映像途切れや「最初の映像を待っている」の場面でメッセージが出ない。
void hold_stalled_frames(SimPoll &sim)
{
	const ledger::Observation &obs = sim.obs();
	std::vector<std::string> ids;
	for (const ledger::PersonFact &p : obs.people) {
		if (!p.slot.has_value() || !p.sharing)
			continue;
		if (!p.last_video_frame_at_us.has_value() || obs.now_us - *p.last_video_frame_at_us >= obs.video_stall_threshold_us)
			ids.push_back(p.user_id);
	}
	for (const std::string &id : ids)
		sim.set_frames_stopped(id, true);
}

// 場面を始めた SimPoll を始める。メッセージは待たずにすぐ表示し、ドックを (width x height) にして、tab を開く。
void open_dock(SimPoll &sim, Input &input, int width, int height, config::DockTab tab)
{
	sim.settings().notice_show_delay_ms = 0;
	sim.settings().layout_hold_idle_ms = 600;
	sim.dock()->setGeometry(1300, 120, width, height);
	sim.widget()->set_tab(tab);
	sim.dock()->show();
	input.activate(sim.dock());
	sim.start();
	pump(500);
}

// 参加者タブの行の矩形 (スクリーン座標。ユーザー ID の順)。
std::map<std::string, QRect> row_rects(SimPoll &sim)
{
	std::map<std::string, QRect> out;
	dock::PeopleList *list = sim.people();
	if (list == nullptr)
		return out;
	for (const std::string &id : list->shown_user_ids()) {
		if (dock::PersonRowWidget *row = list->row_for(id))
			out[id] = global_rect(row);
	}
	return out;
}

// タブのバーの 4 つのタブの矩形 (スクリーン座標)。
std::vector<QRect> tab_rects(SimPoll &sim)
{
	std::vector<QRect> out;
	auto *bar = descendant_of<dock::DockTabBar>(sim.widget());
	if (bar == nullptr)
		return out;
	for (int i = 0; i < bar->count(); ++i)
		out.push_back(QRect(bar->mapToGlobal(bar->tabRect(i).topLeft()), bar->tabRect(i).size()));
	return out;
}

QString rect_text(const QRect &q)
{
	return QStringLiteral("(%1,%2 %3x%4)").arg(q.x()).arg(q.y()).arg(q.width()).arg(q.height());
}

// 1 つの場面を表示したときの、動かしてはいけない座標。
struct Placement {
	bool ok = false;
	bool has_sub = false;
	bool has_notices = false;
	Frame frame;
	std::vector<QRect> tabs;
	int min_width = 0;
	int width = 0;
	std::map<std::string, QRect> rows;
	dv::PeopleListView list;
};

Placement place_scene(const ui_preview::Fixture &fixture, const dock::ui::DockStyle &style, int width, int height)
{
	Placement p;
	SimPoll sim(fixture, style);
	Input input;
	hold_stalled_frames(sim);
	open_dock(sim, input, width, height, config::DockTab::People);
	p.ok = true;
	p.has_sub = sim.view().accounts.sub.has_value();
	p.has_notices = !sim.view().notices.empty();
	p.frame = frame_of(sim.widget());
	p.tabs = tab_rects(sim);
	p.min_width = sim.widget()->computed_min_width();
	p.width = sim.widget()->width();
	p.rows = row_rects(sim);
	p.list = sim.view().people_list;
	return p;
}

// ---- (a) メッセージのすべての場面で座標が動かない ----
void run_positions_messages(const ScenarioOptions &options, Results &results)
{
	// メッセージを出す場面を、場面の一覧から機械的に集める。
	std::vector<ui_preview::Fixture> message_scenes;
	std::vector<ui_preview::Fixture> plain_scenes;
	for (const ui_preview::Fixture &f : ui_preview::dock_fixtures()) {
		if (!ui_preview::view_for(f).notices.empty())
			message_scenes.push_back(f);
		else
			plain_scenes.push_back(f);
	}
	results.info(kGroup, QStringLiteral("(a) メッセージを出す場面の数"), QString::number(message_scenes.size()),
		     QStringLiteral("ドックの場面 %1 のうち").arg(message_scenes.size() + plain_scenes.size()));
	results.expect_true(kGroup, QStringLiteral("前提: メッセージを出す場面が 20 以上ある"), message_scenes.size() >= 20);

	// メッセージのない同じ構成の場面 (サブの有無ごと)。平常の場面 status-connected-in-call と、サブがある構成は、
	// メッセージのない場面のうちサブがある最初の場面。
	const std::pair<int, int> sizes[] = {{420, 700}, {300, 560}};
	for (const auto &[width, height] : sizes) {
		const QString at = QStringLiteral("%1x%2").arg(width).arg(height);
		std::map<bool, Placement> baseline; // key: サブがあるか
		std::map<bool, QString> baseline_id;
		{
			Placement main_only = place_scene(ui_preview::fixture_by_id("status-connected-in-call"), options.style, width, height);
			baseline[main_only.has_sub] = main_only;
			baseline_id[main_only.has_sub] = QStringLiteral("status-connected-in-call");
		}
		for (const ui_preview::Fixture &f : plain_scenes) {
			if (baseline.size() == 2)
				break;
			Placement p = place_scene(f, options.style, width, height);
			if (p.has_notices || baseline.count(p.has_sub) != 0)
				continue;
			baseline[p.has_sub] = p;
			baseline_id[p.has_sub] = QString::fromStdString(f.id);
		}
		results.expect_true(kGroup, QStringLiteral("前提: メインだけの構成とサブがある構成の、メッセージのない場面がある (%1)").arg(at),
				    baseline.size() == 2);

		int compared = 0;
		int rows_compared = 0;
		QStringList frame_bad;
		QStringList tabs_bad;
		QStringList width_bad;
		QStringList rows_bad;
		QStringList no_message;
		for (const ui_preview::Fixture &f : message_scenes) {
			const Placement p = place_scene(f, options.style, width, height);
			const auto base = baseline.find(p.has_sub);
			if (base == baseline.end())
				continue;
			const QString id = QString::fromStdString(f.id);
			if (!p.has_notices) {
				no_message << id;
				continue;
			}
			++compared;
			if (p.frame != base->second.frame)
				frame_bad << id + QStringLiteral(" ") + frame_text(p.frame) + QStringLiteral(" != ") + frame_text(base->second.frame);
			if (p.tabs != base->second.tabs)
				tabs_bad << id;
			if (p.min_width != base->second.min_width || p.width != base->second.width)
				width_bad << QStringLiteral("%1 %2/%3 != %4/%5")
						     .arg(id)
						     .arg(p.min_width)
						     .arg(p.width)
						     .arg(base->second.min_width)
						     .arg(base->second.width);
			// 参加者の一覧が同じなら、メッセージだけが違う場面なので、行の座標も同じ。
			if (p.list == base->second.list) {
				++rows_compared;
				if (p.rows != base->second.rows)
					rows_bad << id;
			}
		}
		results.expect_true(kGroup, QStringLiteral("(a) %1: すべてのメッセージの場面を、メッセージのない場面と比べた").arg(at),
				    compared == static_cast<int>(message_scenes.size()) && no_message.isEmpty(),
				    QStringLiteral("比べた %1 / %2、SimPoll でメッセージが出なかった: %3")
					    .arg(compared)
					    .arg(message_scenes.size())
					    .arg(no_message.join(QLatin1Char(' '))));
		results.expect_true(kGroup, QStringLiteral("(a) %1: 1 行目、メインとサブの行、タブのバー、下のバーの座標と大きさが同じ").arg(at),
				    frame_bad.isEmpty(), frame_bad.join(QStringLiteral(" / ")));
		results.expect_true(kGroup, QStringLiteral("(a) %1: 4 つのタブの座標と大きさが同じ").arg(at), tabs_bad.isEmpty(),
				    tabs_bad.join(QLatin1Char(' ')));
		results.expect_true(kGroup, QStringLiteral("(a) %1: ドックの最小の幅と幅が同じ").arg(at), width_bad.isEmpty(),
				    width_bad.join(QStringLiteral(" / ")));
		results.expect_true(kGroup, QStringLiteral("(a) %1: 参加者の一覧が同じ場面は、行の座標も同じ (%2 場面)").arg(at).arg(rows_compared),
				    rows_bad.isEmpty() && rows_compared > 0, rows_bad.join(QLatin1Char(' ')));
		for (const auto &[has_sub, id] : baseline_id)
			results.info(kGroup, QStringLiteral("(a) %1: 比べる元の場面").arg(at), id,
				     has_sub ? QStringLiteral("サブがある") : QStringLiteral("メインだけ"));
	}
}

// ---- (b) 映像途切れの繰り返し ----
void run_positions_stall(const ScenarioOptions &options, Results &results)
{
	SimPoll sim(ui_preview::fixture_by_id("status-connected-in-call"), options.style);
	Input input;
	// 途切れを速く繰り返すため、しきい値と消すまでの待ちを短くする (製品の値は 2 秒と 1 秒)。
	sim.obs().video_stall_threshold_us = 300'000;
	sim.obs().uninstall.obs_streaming = true;
	sim.settings().notice_clear_delay_ms = 100;
	open_dock(sim, input, 420, 700, config::DockTab::People);
	// 枠に載っていて画面共有している人を、回線の弱い人にする。
	std::string weak;
	for (const ledger::PersonFact &p : sim.obs().people) {
		if (p.slot.has_value() && p.sharing && !p.is_receiver) {
			weak = p.user_id;
			break;
		}
	}
	const auto stalled = [&sim, &weak]() {
		for (const dv::PersonRow &row : sim.view().people) {
			if (row.user_id == weak && row.status.has_value() && row.status->state == dv::RowState::NoVideo)
				return true;
		}
		return false;
	};
	results.expect_true(kGroup, QStringLiteral("前提: 枠に載っていて画面共有している人がいて、最初は途切れていない"), !weak.empty() && !stalled());
	if (weak.empty())
		return;
	auto *first = descendant_of<dock::FirstLineBar>(sim.widget());
	const Frame frame0 = frame_of(sim.widget());
	const std::vector<QRect> tabs0 = tab_rects(sim);
	const std::map<std::string, QRect> rows0 = row_rects(sim);
	const QString first_text0 = first_line_full_text(sim.widget());

	int moved = 0;
	int red = 0;
	int notices = 0;
	int samples = 0;
	QString detail;
	const auto sample = [&] {
		++samples;
		if (frame_of(sim.widget()) != frame0 || tab_rects(sim) != tabs0 || row_rects(sim) != rows0) {
			++moved;
			if (detail.isEmpty())
				detail = frame_text(frame_of(sim.widget()));
		}
		if (first->property("l1Surface").toBool() ||
		    (sim.view().first_line.tier.has_value() && *sim.view().first_line.tier == dv::Tier::L1))
			++red;
		if (!sim.view().notices.empty())
			++notices;
	};
	int shown = 0;
	int cleared = 0;
	for (int i = 0; i < 20; ++i) {
		sim.set_frames_stopped(weak, true);
		if (wait_until(stalled, 5000, sample))
			++shown;
		sample();
		sim.set_frames_stopped(weak, false);
		if (wait_until([&] { return !stalled(); }, 5000, sample))
			++cleared;
		sample();
	}
	pump(300);
	sample();
	results.expect_eq(kGroup, QStringLiteral("(b) 「映像途切れ」が 20 回、行に出た"), shown, 20);
	results.expect_eq(kGroup, QStringLiteral("(b) 「映像途切れ」が 20 回、行から消えた"), cleared, 20);
	results.expect_true(kGroup, QStringLiteral("(b) 20 回の間 (%1 回測った)、1 行目、タブ、行の座標が 1 度も動かない").arg(samples), moved == 0,
			    detail);
	results.expect_eq(kGroup, QStringLiteral("(b) 20 回の間、1 行目がメッセージのレベル 1 の見た目にならない"), red, 0);
	results.expect_eq(kGroup, QStringLiteral("(b) 20 回の間、メッセージが 1 つも出ない (1 人の途切れは行だけで伝える)"), notices, 0);
	results.expect_true(kGroup, QStringLiteral("(b) 1 行目の文が変わらない"), first_line_full_text(sim.widget()) == first_text0,
			    first_line_full_text(sim.widget()));

	// 状態タブの累計。
	int count = -1;
	for (const dv::StatusReceiveRow &row : sim.view().status_tab.rows) {
		if (row.user_id == weak)
			count = static_cast<int>(row.gap_count);
	}
	results.expect_eq(kGroup, QStringLiteral("(b) 状態タブの累計が 20 回"), count, 20);
	sim.widget()->set_tab(config::DockTab::Status);
	pump(800);
	dock::StatusTab *tab = sim.widget()->status_tab();
	QString weak_name;
	for (const dv::StatusReceiveRow &row : sim.view().status_tab.rows) {
		if (row.user_id == weak)
			weak_name = QString::fromStdString(row.name.literal);
	}
	QString cell;
	for (int r = 1; tab != nullptr && r <= tab->table_rows(); ++r) {
		auto *gaps = qobject_cast<QLabel *>(tab->cell(r, 1));
		if (gaps != nullptr && !weak_name.isEmpty() && gaps->accessibleName().startsWith(weak_name))
			cell = gaps->text();
	}
	results.expect_true(kGroup, QStringLiteral("(b) 状態タブの表の途切れの列に 20 がある"), cell.contains(QStringLiteral("20")), cell);
	results.expect_true(kGroup, QStringLiteral("(b) 状態タブを開いても、1 行目、タブ、下のバーの座標が動かない"),
			    frame_of(sim.widget()) == frame0 && tab_rects(sim) == tabs0);
}

// ---- (f) 枠に載せた人だけ: ずっと前から共有している人のチェックを押したとき ----
// 受け取る範囲の既定は「枠に載せた人だけ」で、チェックを押すまで映像は届かない。ずっと前から共有している人のチェックを
// 押した瞬間に、待ち時間を超えたことにして、映像が届かないメッセージを出してはいけない (押すたびに出るため)。
void run_positions_first_frame(const ScenarioOptions &options, Results &results)
{
	ui_preview::Fixture fixture = ui_preview::fixture_by_id("status-connected-in-call");
	std::vector<std::string> sharers;
	for (std::size_t i = 0; i < fixture.obs.people.size(); ++i) {
		ledger::PersonFact &p = fixture.obs.people[i];
		if (p.is_receiver)
			continue;
		if (sharers.size() < 2) {
			// 10 分前から共有している。まだ枠に載せていないので、映像は届いていない。
			p.sharing = true;
			p.sharing_known = true;
			p.sharing_since_us = fixture.obs.now_us - 600'000'000;
			p.slot.reset();
			p.last_video_frame_at_us.reset();
			p.watching_since_us.reset();
			sharers.push_back(p.user_id);
		} else {
			// 残りの人は、映像が届いている人にしない (届いている人がいると、メッセージは出ない)。
			p.slot.reset();
			p.sharing = false;
			p.last_video_frame_at_us.reset();
		}
	}
	SimPoll sim(fixture, options.style);
	Input input;
	sim.obs().choices.receive_scope = ledger::ReceiveScope::Slots;
	sim.settings().first_frame_wait_ms = 3000;
	sim.settings().notice_clear_delay_ms = 100;
	for (const std::string &id : sharers)
		sim.set_frames_stopped(id, true);
	open_dock(sim, input, 420, 700, config::DockTab::People);
	results.expect_eq(kGroup, QStringLiteral("(f) 前提: 10 分前から共有している人が 2 人いる"), static_cast<int>(sharers.size()), 2);
	if (sharers.size() < 2)
		return;

	const auto waiting_rows = [&sim, &sharers]() {
		int n = 0;
		for (const dv::PersonRow &row : sim.view().people) {
			if (std::find(sharers.begin(), sharers.end(), row.user_id) != sharers.end() && row.status.has_value() &&
			    row.status->state == dv::RowState::Waiting)
				++n;
		}
		return n;
	};
	results.expect_eq(kGroup, QStringLiteral("(f) 前提: チェックを押す前は、どちらの行にも状態もメッセージも出ない"),
			  waiting_rows() + static_cast<int>(sim.view().notices.size()), 0);

	const Frame frame0 = frame_of(sim.widget());
	const std::vector<QRect> tabs0 = tab_rects(sim);
	const std::map<std::string, QRect> rows0 = row_rects(sim);
	int moved = 0;
	int early_notices = 0;
	const auto sample = [&] {
		if (frame_of(sim.widget()) != frame0 || tab_rects(sim) != tabs0 || row_rects(sim) != rows0)
			++moved;
		if (!sim.view().notices.empty())
			++early_notices;
	};

	// 2 人のチェックを、押し間違いを防ぐ待ちを置いて押す (どちらも製品と同じ操作)。
	std::int64_t first_click_ms = 0;
	for (std::size_t i = 0; i < sharers.size(); ++i) {
		const QRect hit = check_hit_rect_global(sim.people(), sharers[i]);
		input.move_to(hit.center());
		wait_until([] { return false; }, 800, sample);
		if (i == 0)
			first_click_ms = sim.elapsed_ms();
		input.click();
		sample();
	}
	results.expect_true(kGroup, QStringLiteral("(f) 2 人とも枠に載った"), [&] {
		int n = 0;
		for (const ledger::PersonFact &p : sim.obs().people) {
			if (std::find(sharers.begin(), sharers.end(), p.user_id) != sharers.end() && p.slot.has_value())
				++n;
		}
		return n == 2;
	}());
	results.expect_true(kGroup, QStringLiteral("(f) チェックを押すと、どちらの行も「映像待ち」になる"),
			    wait_until([&] { return waiting_rows() == 2; }, 3000, sample));

	// 待ち時間 (3000 ms。1 人目のチェックから数える) の手前までは、メッセージが出ない。
	wait_until([&] { return sim.elapsed_ms() - first_click_ms >= 2400; }, 4000, sample);
	results.expect_eq(kGroup, QStringLiteral("(f) 待ち時間の手前まで、メッセージが 1 つも出ない"), early_notices, 0);
	results.expect_eq(kGroup, QStringLiteral("(f) 待ち時間の手前まで、2 人とも「映像待ち」のまま"), waiting_rows(), 2);

	// 待ち時間を超えても届かなければ、メッセージが出る。出るのは待ち時間の後。
	const bool shown = wait_until([&sim] { return !sim.view().notices.empty(); }, 4000);
	const std::int64_t shown_after_ms = sim.elapsed_ms() - first_click_ms;
	results.expect_true(kGroup, QStringLiteral("(f) 待ち時間を超えても届かなければ、メッセージが出る"), shown);
	results.expect_true(kGroup, QStringLiteral("(f) メッセージは、1 人目のチェックから 2900 ms 以上たってから出る (%1 ms)").arg(shown_after_ms),
			    shown_after_ms >= 2900);

	// 最初の映像が届くと、メッセージも行の状態も消える。
	for (const std::string &id : sharers)
		sim.set_frames_stopped(id, false);
	const bool cleared = wait_until([&] { return sim.view().notices.empty() && waiting_rows() == 0; }, 5000, sample);
	results.expect_true(kGroup, QStringLiteral("(f) 最初の映像が届くと、メッセージと行の状態が消える"), cleared);
	pump(300);
	sample();
	results.expect_eq(kGroup, QStringLiteral("(f) 押してから映像が届くまで、1 行目、タブ、行の座標が動かない"), moved, 0);
}

// ---- 30 人と 235x300 で使う関数 ----

// scroll の見える範囲 (スクリーン座標)。
QRect viewport_rect(QScrollArea *scroll)
{
	return QRect(scroll->viewport()->mapToGlobal(QPoint(0, 0)), scroll->viewport()->size());
}

// スクロールの中の、表示している子孫で、見える幅より右にはみ出すものの数 (中身の幅が見える幅を超えるときも数える)。
int overflow_count(QScrollArea *scroll, QString *detail)
{
	const int right = scroll->viewport()->mapToGlobal(QPoint(scroll->viewport()->width(), 0)).x();
	int count = 0;
	QStringList names;
	for (QWidget *w : scroll->widget()->findChildren<QWidget *>()) {
		if (!w->isVisibleTo(scroll->widget()))
			continue;
		const int w_right = w->mapToGlobal(QPoint(w->width(), 0)).x();
		if (w_right > right + 1) {
			++count;
			if (names.size() < 4)
				names << QStringLiteral("%1(%2)").arg(QString::fromLatin1(w->metaObject()->className()), w->objectName());
		}
	}
	if (scroll->widget()->width() > scroll->viewport()->width()) {
		++count;
		names << QStringLiteral("content %1 > viewport %2").arg(scroll->widget()->width()).arg(scroll->viewport()->width());
	}
	if (scroll->horizontalScrollBar()->isVisible() && scroll->horizontalScrollBar()->maximum() > 0) {
		++count;
		names << QStringLiteral("horizontal scroll");
	}
	if (detail != nullptr)
		*detail = names.join(QLatin1Char(' '));
	return count;
}

// スクロールを先頭から最後まで動かして、targets のうち、見える範囲に中心が入った数を返す。
int reached_by_scrolling(QScrollArea *scroll, const std::vector<QWidget *> &targets)
{
	std::set<QWidget *> reached;
	QScrollBar *bar = scroll->verticalScrollBar();
	const int step = std::max(20, scroll->viewport()->height() / 2);
	bar->setValue(0);
	pump(30);
	for (int v = 0;; v += step) {
		bar->setValue(std::min(v, bar->maximum()));
		pump(20);
		const QRect view = viewport_rect(scroll);
		for (QWidget *w : targets) {
			if (w != nullptr && w->isVisibleTo(scroll->widget()) && view.contains(center_of(w)))
				reached.insert(w);
		}
		if (v >= bar->maximum())
			break;
	}
	bar->setValue(0);
	pump(30);
	return static_cast<int>(reached.size());
}

// w の中心にポインターを置いてホイールを回す。スクロールした量を返す。
int wheel_over(Input &input, QScrollArea *scroll, QWidget *w)
{
	QScrollBar *bar = scroll->verticalScrollBar();
	bar->setValue(0);
	scroll->ensureWidgetVisible(w, 0, 0);
	pump(80);
	const int before = bar->value();
	input.move_to(center_of(w));
	pump(60);
	for (int i = 0; i < 3; ++i) {
		input.wheel(-120);
		pump(40);
	}
	pump(150);
	return bar->value() - before;
}

// ---- (c) 30 人 ----
void run_positions_thirty(const ScenarioOptions &options, Results &results)
{
	struct Case {
		const char *scene;
		config::DockTab tab;
		const char *name;
	};
	const Case cases[] = {{"people-30", config::DockTab::People, "参加者タブ"},
			      {"audio-tab-30", config::DockTab::Audio, "音声タブ"},
			      {"status-table-30", config::DockTab::Status, "状態タブ"}};
	for (const Case &c : cases) {
		for (const bool at_min : {false, true}) {
			SimPoll sim(ui_preview::fixture_by_id(c.scene), options.style);
			Input input;
			sim.arrival() = sim.obs().receive.arrival;
			open_dock(sim, input, 420, 700, c.tab);
			if (at_min) {
				sim.dock()->setGeometry(1300, 120, sim.widget()->computed_min_width() + 2, 700);
				pump(600);
			}
			const QString where = QStringLiteral("(c) %1 (%2)").arg(QString::fromUtf8(c.name), at_min ? QStringLiteral("最小の幅") : QStringLiteral("420 px"));
			QScrollArea *scroll = scroll_of(sim, scroll_name_of(c.tab));
			results.expect_true(kGroup, where + QStringLiteral(": 前提: スクロール領域がある"), scroll != nullptr);
			if (scroll == nullptr)
				continue;
			QString detail;
			results.expect_eq(kGroup, where + QStringLiteral(": 横にはみ出さない"), overflow_count(scroll, &detail), 0, detail);

			// スクロールで全員に届く。
			std::vector<QWidget *> targets;
			std::vector<QWidget *> value_widgets;
			if (c.tab == config::DockTab::People) {
				for (const dv::PersonRow &row : listed_rows(sim.view())) {
					if (QWidget *w = sim.people()->row_for(row.user_id))
						targets.push_back(w);
				}
			} else if (c.tab == config::DockTab::Audio) {
				for (const std::string &id : sim.audio()->shown_user_ids()) {
					if (QWidget *w = sim.audio()->row_for(id)) {
						targets.push_back(w);
						value_widgets.push_back(sim.audio()->row_for(id)->slider(dock::AudioKind::Voice));
					}
				}
			} else {
				dock::StatusTab *tab = sim.widget()->status_tab();
				for (int r = 1; tab != nullptr && r <= tab->table_rows(); ++r)
					targets.push_back(tab->cell(r, 0));
				if (tab != nullptr)
					value_widgets.push_back(tab->buffer_input()->slider());
			}
			const int expected = c.tab == config::DockTab::People ? static_cast<int>(listed_rows(sim.view()).size()) : 30;
			results.expect_true(kGroup, where + QStringLiteral(": 前提: 全員の行がある (%1 / %2)").arg(targets.size()).arg(expected),
					    static_cast<int>(targets.size()) >= expected);
			results.expect_eq(kGroup, where + QStringLiteral(": スクロールで全員に届く"), reached_by_scrolling(scroll, targets),
					  static_cast<long long>(targets.size()));

			// ホイールで値が変わらない (行や値を押さない)。
			const size_t actions0 = sim.actions().size();
			const int reflects0 = sim.look_reflects() + sim.buffer_reflects();
			QWidget *over = !value_widgets.empty() ? value_widgets.front() : (targets.empty() ? nullptr : targets.front());
			const int voice0 = c.tab == config::DockTab::Audio ? sim.audio()->row_for(sim.audio()->shown_user_ids().front())
										      ->slider(dock::AudioKind::Voice)
										      ->value()
									  : 0;
			const int moved = over != nullptr ? wheel_over(input, scroll, over) : 0;
			const int voice1 = c.tab == config::DockTab::Audio ? sim.audio()->row_for(sim.audio()->shown_user_ids().front())
										      ->slider(dock::AudioKind::Voice)
										      ->value()
									  : 0;
			results.expect_true(kGroup, where + QStringLiteral(": ホイールで値が変わらない (操作が通らず、音量も同じ)"),
					    sim.actions().size() == actions0 && sim.look_reflects() + sim.buffer_reflects() == reflects0 && voice0 == voice1,
					    QStringLiteral("操作 %1 -> %2、音量 %3 -> %4")
						    .arg(actions0)
						    .arg(sim.actions().size())
						    .arg(voice0)
						    .arg(voice1));
			results.expect_true(kGroup, where + QStringLiteral(": ホイールでスクロールする"), moved > 0, QString::number(moved));
		}
	}
}

// ---- (d) 235x300 ----
void run_positions_tiny(const ScenarioOptions &options, Results &results)
{
	struct Case {
		const char *scene;
		config::DockTab tab;
		const char *name;
	};
	const Case cases[] = {{"people-8", config::DockTab::People, "参加者タブ"},
			      {"people-30", config::DockTab::People, "参加者タブ (30 人)"},
			      {"audio-tab-30", config::DockTab::Audio, "音声タブ"},
			      {"status-messages", config::DockTab::Status, "状態タブ"},
			      {"notice-tiers-overlap", config::DockTab::Status, "状態タブ (メッセージの重なり)"}};
	for (const Case &c : cases) {
		SimPoll sim(ui_preview::fixture_by_id(c.scene), options.style);
		Input input;
		open_dock(sim, input, 235, 300, c.tab);
		const QString where = QStringLiteral("(d) 235x300 %1").arg(QString::fromUtf8(c.name));
		const int min_w = sim.widget()->computed_min_width();
		const int actual = sim.widget()->width();
		results.info(kGroup, where + QStringLiteral(": 最小の幅と実際の幅"), QStringLiteral("%1 / %2").arg(min_w).arg(actual));
		results.expect_true(kGroup, where + QStringLiteral(": 実際の幅が最小の幅になる"), actual == min_w,
				    QStringLiteral("最小 %1、実際 %2").arg(min_w).arg(actual));
		QScrollArea *scroll = scroll_of(sim, scroll_name_of(c.tab));
		const int h = scroll != nullptr ? scroll->viewport()->height() : 0;
		results.expect_true(kGroup, where + QStringLiteral(": 一覧の領域の高さが 0 にならない (%1 px)").arg(h), h > 0);
		QString detail;
		results.expect_eq(kGroup, where + QStringLiteral(": 横にはみ出さない"), scroll != nullptr ? overflow_count(scroll, &detail) : -1, 0, detail);

		// タブのバーを押してタブが替わる。
		auto *bar = descendant_of<dock::DockTabBar>(sim.widget());
		const std::vector<QRect> tabs = tab_rects(sim);
		bool tabs_ok = bar != nullptr && tabs.size() == 4;
		// タブのバーの並び (参加者、音声、表示、状態)。
		const config::DockTab order[] = {config::DockTab::People, config::DockTab::Audio, config::DockTab::Display, config::DockTab::Status};
		QStringList bad;
		for (int i = 0; tabs_ok && i < 4; ++i) {
			const int target = (i + 1) % 4;
			input.move_to(tabs[static_cast<size_t>(target)].center());
			pump(40);
			input.click();
			pump(300);
			if (sim.widget()->tab() != order[target])
				bad << QString::number(target);
		}
		results.expect_true(kGroup, where + QStringLiteral(": 4 つのタブを押せる"), tabs_ok && bad.isEmpty(), QStringLiteral("押せなかったタブ: ") + bad.join(QLatin1Char(' ')));

		// 下のバー。録画のボタンと [...] が、ドックの中にあって、その場所を押すとそれぞれのボタンに届く。
		auto *bottom = descendant_of<dock::BottomBar>(sim.widget());
		const QRect dock_rect = global_rect(sim.widget());
		bool bottom_ok = bottom != nullptr;
		QString bottom_detail;
		for (QWidget *button : {static_cast<QWidget *>(bottom != nullptr ? bottom->record_button() : nullptr),
					static_cast<QWidget *>(bottom != nullptr ? bottom->menu_button() : nullptr)}) {
			if (button == nullptr || !button->isVisible()) {
				bottom_ok = false;
				continue;
			}
			const QRect r = global_rect(button);
			QWidget *hit = Input::widget_at(r.center());
			const bool reach = hit != nullptr && (hit == button || button->isAncestorOf(hit));
			if (!reach || !dock_rect.contains(r) || r.width() <= 0 || r.height() <= 0) {
				bottom_ok = false;
				bottom_detail += rect_text(r) + QLatin1Char(' ');
			}
		}
		results.expect_true(kGroup, where + QStringLiteral(": 下のバーの録画のボタンと [...] を押せる"), bottom_ok, bottom_detail);
	}
}

// ---- (e) メッセージの重なり ----
void run_positions_overlap(const ScenarioOptions &options, Results &results)
{
	std::map<bool, Placement> baseline;
	for (const char *id : {"status-connected-in-call", "display-default"}) {
		Placement p = place_scene(ui_preview::fixture_by_id(id), options.style, 420, 700);
		if (!p.has_notices && baseline.count(p.has_sub) == 0)
			baseline[p.has_sub] = p;
	}
	for (const char *id : {"notice-tiers-overlap", "notice-many", "notice-l1-two", "status-messages", "notice-l1-space-finalize-failed"}) {
		SimPoll sim(ui_preview::fixture_by_id(id), options.style);
		Input input;
		hold_stalled_frames(sim);
		open_dock(sim, input, 420, 700, config::DockTab::People);
		const QString where = QStringLiteral("(e) %1").arg(QString::fromLatin1(id));
		int tier_count[3] = {0, 0, 0};
		for (const dv::Notice &n : sim.view().notices)
			++tier_count[static_cast<int>(n.tier)];
		results.info(kGroup, where + QStringLiteral(": レベル 1、2、3 の件数"),
			     QStringLiteral("%1 / %2 / %3").arg(tier_count[0]).arg(tier_count[1]).arg(tier_count[2]));
		if (std::string(id) == "notice-tiers-overlap")
			results.expect_true(kGroup, where + QStringLiteral(": 前提: レベル 1、2、3 がそれぞれ 2 件以上ある"),
					    tier_count[0] >= 2 && tier_count[1] >= 2 && tier_count[2] >= 2);

		// 1 行目は 1 つだけ。見出しは先頭のメッセージの 1 つで、残りは件数にする。
		auto *first = descendant_of<dock::FirstLineBar>(sim.widget());
		const dv::FirstLine &line = sim.view().first_line;
		const bool single = line.kind == dv::FirstLineKind::Notice && !sim.view().notices.empty() &&
				    line.notice_key == sim.view().notices.front().key &&
				    first_line_full_text(sim.widget()).contains(first_line_expected(sim.view())) &&
				    line.more_count.has_value() == (tier_count[0] > 1);
		results.expect_true(kGroup, where + QStringLiteral(": 1 行目は 1 つの見出しだけで、残りのレベル 1 は件数にする"), single,
				    first_line_full_text(sim.widget()));
		const bool red = first->property("l1Surface").toBool();
		results.expect_true(kGroup, where + QStringLiteral(": レベル 1 があるときだけ 1 行目をレベル 1 の見た目にする"), red == (tier_count[0] > 0));

		// 座標は動かない。
		const auto base = baseline.find(sim.view().accounts.sub.has_value());
		if (base != baseline.end()) {
			results.expect_true(kGroup, where + QStringLiteral(": 1 行目、メインとサブの行、タブのバー、下のバーの座標が動かない"),
					    frame_of(sim.widget()) == base->second.frame && tab_rects(sim) == base->second.tabs,
					    frame_of(sim.widget()) == base->second.frame ? QString() : frame_text(frame_of(sim.widget())) + QStringLiteral(" != ") + frame_text(base->second.frame));
		}
		const Frame frame0 = frame_of(sim.widget());

		// 状態タブにすべてある。
		sim.widget()->set_tab(config::DockTab::Status);
		pump(800);
		const QStringList shown = shown_notice_keys(sim.widget());
		const QStringList expected = expected_notice_keys(sim.view());
		results.expect_true(kGroup, where + QStringLiteral(": 状態タブにすべてのメッセージが、レベルの順にある (%1 件)").arg(expected.size()),
				    shown == expected && !expected.isEmpty(),
				    shown == expected ? QString() : shown.join(QLatin1Char(' ')) + QStringLiteral(" != ") + expected.join(QLatin1Char(' ')));
		results.expect_true(kGroup, where + QStringLiteral(": 状態タブを開いても座標が動かない"), frame_of(sim.widget()) == frame0);
	}
}

// ---- (g) ドックの更新 1 回あたりの時間 ----
// 合否を持たない。ドックの更新間隔の既定値を決める材料として、更新 1 回の時間を測って info で出す。
// 更新 1 回 = build_observation (Pipeline の snapshot と OS の問い合わせの結果から Observation を作る) + decide_dock
// + 描画 (SimPoll::tick が行う、decide_dock、メッセージの待ち、1 行目、状態タブ、DockWidget::render)。
// 測れないもの: Pipeline::snapshot() (libobs と Pipeline の実体が要る。dock_drive には入れられない)。snapshot() は
// 周期ごとに 1 回、ロックを取って map と vector をコピーする処理なので、実機の値は別に測る必要がある。
// build_observation の入力 (snapshot) は、場面の人数と同じ人数の参加者を持つ合成の値である。

pipeline::PipelineSnapshot synthetic_snapshot(int people, std::int64_t wall_now_us)
{
	pipeline::PipelineSnapshot snapshot;
	snapshot.taken_at_wall_us = wall_now_us;
	snapshot.taken_at_steady_us = 600'000'000;
	snapshot.listener.client_connected = true;
	snapshot.facts.received = true;
	snapshot.profiles.received = true;
	pipeline::UserIdSet roster;
	pipeline::UserIdSet sharing;
	for (int i = 0; i < people; ++i) {
		const std::string id = "user_" + std::to_string(1000 + i);
		roster.user_ids.push_back(id);
		pipeline::ProfileInfo profile;
		profile.display_name = "Person " + std::to_string(i);
		profile.username = "person_" + std::to_string(i);
		profile.avatar_key = "";
		snapshot.profiles.people[id] = profile;
		if (i % 3 == 0) {
			sharing.user_ids.push_back(id);
			snapshot.facts.sharing_since_us[id] = wall_now_us - 60'000'000;
			pipeline::StreamFormat format;
			format.stream_id = static_cast<std::uint32_t>(100 + i);
			format.user_id = id;
			format.screen = true;
			format.width = 1920;
			format.height = 1080;
			format.fps = 30.0;
			format.audio = true;
			snapshot.stream_formats[format.stream_id] = format;
			pipeline::StreamFrameClock clock;
			clock.stream_id = format.stream_id;
			clock.user_id = id;
			clock.kind = config::SlotKind::Screen;
			clock.last_video_frame_steady_us = snapshot.taken_at_steady_us - 20'000;
			clock.last_video_frame_wall_us = wall_now_us - 20'000;
			snapshot.last_frames[format.stream_id] = clock;
		}
		pipeline::StreamFormat voice;
		voice.stream_id = static_cast<std::uint32_t>(500 + i);
		voice.user_id = id;
		snapshot.stream_formats[voice.stream_id] = voice;
	}
	roster.sent_count = static_cast<int>(roster.user_ids.size());
	sharing.sent_count = static_cast<int>(sharing.user_ids.size());
	snapshot.facts.roster = roster;
	snapshot.facts.sharing = sharing;
	snapshot.facts.sharing_valid = true;
	return snapshot;
}

double percentile_ms(std::vector<double> samples, double fraction)
{
	if (samples.empty())
		return 0.0;
	std::sort(samples.begin(), samples.end());
	const std::size_t index = std::min(samples.size() - 1, static_cast<std::size_t>(fraction * (samples.size() - 1) + 0.5));
	return samples[index];
}

void run_positions_refresh_cost(const ScenarioOptions &options, Results &results)
{
	struct Case {
		const char *scene;
		config::DockTab tab;
		int people;
		const char *label;
	};
	const Case cases[] = {{"status-connected-in-call", config::DockTab::People, 4, "平常 (4 人、参加者タブ)"},
			      {"people-8", config::DockTab::People, 8, "8 人、参加者タブ"},
			      {"people-30", config::DockTab::People, 30, "30 人、参加者タブ"},
			      {"audio-tab-30", config::DockTab::Audio, 30, "30 人、音声タブ"},
			      {"status-table-30", config::DockTab::Status, 30, "30 人、状態タブ"}};
	constexpr int kWarmup = 20;
	constexpr int kSamples = 200;
	using Clock = std::chrono::steady_clock;
	const auto ms_since = [](Clock::time_point from) {
		return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
	};
	for (const Case &c : cases) {
		SimPoll sim(ui_preview::fixture_by_id(c.scene), options.style);
		Input input;
		open_dock(sim, input, 420, 700, c.tab);
		const dock::DockFacts facts = [&sim] {
			dock::DockFacts f;
			f.now_us = sim.now_us();
			return f;
		}();
		const pipeline::PipelineSnapshot snapshot = synthetic_snapshot(c.people, facts.now_us);
		const platform::OsProbeResult probe;
		// steady は、何も変わらない更新 (ほとんどの更新)。changing は、3 人の共有の有無が毎回入れ替わる更新 (行が組を移り、
		// 描画の書き換えが最も多い)。
		for (const bool changing : {false, true}) {
			std::vector<double> total;
			std::vector<double> build;
			for (int i = 0; i < kWarmup + kSamples; ++i) {
				if (changing) {
					int flipped = 0;
					for (ledger::PersonFact &p : sim.obs().people) {
						if (p.is_receiver)
							continue;
						p.sharing = !p.sharing;
						if (++flipped == 3)
							break;
					}
				}
				const auto start = Clock::now();
				const ledger::Observation built = dock::build_observation(snapshot, probe, facts);
				const double build_ms = ms_since(start);
				sim.tick();
				const double all_ms = ms_since(start);
				if (i >= kWarmup) {
					total.push_back(all_ms);
					build.push_back(build_ms);
				}
				if (built.people.size() > 100000)
					results.info(kGroup, QStringLiteral("(g) 想定外"), QStringLiteral("人数"));
			}
			const QString item = QStringLiteral("(g) 更新 1 回の時間 [%1, %2]").arg(QString::fromUtf8(c.label), changing ? QStringLiteral("変化あり") : QStringLiteral("変化なし"));
			const auto fmt = [](double v) { return QString::number(v, 'f', 3); };
			results.info(kGroup, item,
				     QStringLiteral("中央値 %1 ms, p95 %2 ms, 最大 %3 ms").arg(fmt(percentile_ms(total, 0.5)), fmt(percentile_ms(total, 0.95)), fmt(percentile_ms(total, 1.0))),
				     QStringLiteral("build_observation の p95 は %1 ms。Pipeline::snapshot() は含まない。%2 回").arg(fmt(percentile_ms(build, 0.95))).arg(kSamples));
		}
		sim.stop();
		sim.dock()->hide();
	}
}

} // namespace

void run_positions(const ScenarioOptions &options, Results &results)
{
	run_positions_messages(options, results);
	run_positions_stall(options, results);
	run_positions_first_frame(options, results);
	run_positions_thirty(options, results);
	run_positions_tiny(options, results);
	run_positions_overlap(options, results);
	run_positions_refresh_cost(options, results);
}

} // namespace cordmux::dock_drive
