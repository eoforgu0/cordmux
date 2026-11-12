// SPDX-License-Identifier: Apache-2.0

// summary のシナリオ。平常の 1 行の「表示中」の人数が、規則で数えた人数と同じで、エンコードの段階が unavailable の人を数えない
// こと。
// [ツール] の「cordmuxをアンインストール...」は、設定ウィンドウを開く処理 (Host) が必要なので、scen_windows.cpp の
// run_tools_uninstall で確認する。期待する値はどれも、このファイルのコメントに書いた規則から作り、製品の判定の関数の
// 返り値からは作らない (製品の関数が誤っていても合格するのを避けるため)。

#include "scenarios.h"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAccessible>
#include <QApplication>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QWidget>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dock/dock_widget.h"
#include "dock/first_line.h"
#include "dock/notice_list.h"
#include "dock/people_list.h"
#include "dock/person_row.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/wrap_label.h"
#include "drive_env.h"
#include "input.h"
#include "ledger/plural.h"
#include "ledger/text_ids.h"
#include "obs_dock.h"
#include "query.h"
#include "scen_common.h"
#include "script.h"
#include "sim_poll.h"

namespace cordmux::dock_drive {

namespace {

namespace keys = ledger::text::keys;

void click_widget(Input &input, QWidget *w)
{
	if (w == nullptr)
		return;
	input.move_to(w->mapToGlobal(w->rect().center()));
	input.click();
}

// ---- summary ----

// 「表示中」に数える人の規則。枠に載っていて、画面共有していて (分からない間は、
// フレームが届いていることを根拠に数える)、送信側のエンコードの段階が unavailable (その人の映像を送らない段階) でなく、
// フレームが止まったとみなすしきい値の内に届いている人。エンコードの段階が unavailable の人は、送信側がその人の映像の
// エンコードをやめて送っていないので、フレームの時刻によらず数えない (行が「映像を表示できません」と表示している間に、
// 1 行目がその人を数えないように)。知らない名前の段階の人と、段階のない人は数える (画質を下げた映像か、普通の映像が
// 映っているとみなす)。段階の名前は ledger/observation.h の kStageUnavailable と比べる。製品の displayed() を呼ばずに、
// 規則から数える。
int shown_by_rule(const ledger::Observation &obs, std::vector<std::string> *who = nullptr)
{
	int n = 0;
	for (const ledger::PersonFact &p : obs.people) {
		if (!p.slot.has_value())
			continue;
		if (p.sharing_known && !p.sharing)
			continue;
		if (p.stage.has_value() && *p.stage == ledger::kStageUnavailable)
			continue;
		if (!p.last_video_frame_at_us.has_value() || obs.now_us - *p.last_video_frame_at_us >= obs.video_stall_threshold_us)
			continue;
		++n;
		if (who != nullptr)
			who->push_back(p.user_id);
	}
	return n;
}

// 平常の 1 行の最も長い形 (「通話中 (%1人) - 表示中 (%2人)」)。表示中が 0 人なら別のテキストなので、その形も作る。
QString summary_text(const ledger::Observation &obs, int shown)
{
	const long long n = obs.call.roster_count.value_or(0);
	ledger::TextRef r;
	if (shown > 0) {
		r.id = ledger::text::pick(keys::kDockSummaryCallStatus, n);
		r.args = {std::to_string(n), std::to_string(shown)};
	} else {
		r.id = ledger::text::pick(keys::kDockSummaryNoneDisplayedStatus, n);
		r.args = {std::to_string(n)};
	}
	return dock::ui::resolve(r);
}

// View が持つ平常の 1 行 (メッセージが 1 つも表示されていないときの 1 行目。fallback_line) の最も長い形のテキスト。
QString summary_of_view(const ledger::dockview::DockViewV2 &view)
{
	return view.fallback_line.forms.empty() ? QString() : dock::ui::resolve(view.fallback_line.forms.front());
}

// 行の中で表示されている、状態のアイコンと状態のテキスト。どちらも表示されていなければ空を返す。
QString row_status_shown(dock::PersonRowWidget *row)
{
	if (row == nullptr)
		return QStringLiteral("(no row)");
	QStringList out;
	if (QWidget *mark = row->findChild<QWidget *>(QStringLiteral("cordmuxRowStatus")); mark != nullptr && mark->isVisibleTo(row)) {
		auto *label = qobject_cast<QLabel *>(mark);
		if (label == nullptr || !label->pixmap().isNull())
			out << QStringLiteral("mark");
	}
	if (auto *text = dynamic_cast<dock::ui::ElideLabel *>(row->findChild<QWidget *>(QStringLiteral("cordmuxRowStatusText")));
	    text != nullptr && text->isVisibleTo(row) && !text->full_text().isEmpty())
		out << QStringLiteral("text=") + text->full_text();
	return out.join(QLatin1Char(' '));
}

// 行のウィジェット (行そのものと子のすべて) のツールチップとアクセシブルな説明のうち、needle を含むものが
// それぞれあるか。
struct RowTexts {
	bool tip = false;
	bool desc = false;
};
RowTexts row_texts_contain(dock::PersonRowWidget *row, const QString &needle)
{
	RowTexts r;
	if (row == nullptr)
		return r;
	std::vector<QWidget *> all{row};
	for (QWidget *w : row->findChildren<QWidget *>())
		all.push_back(w);
	for (QWidget *w : all) {
		r.tip = r.tip || w->toolTip().contains(needle);
		r.desc = r.desc || w->accessibleDescription().contains(needle);
	}
	return r;
}

} // namespace

// summary: 平常の 1 行 (メッセージがないときの 1 行目) の「表示中」の人数が、上の規則で数えた人数と同じであることを確認する。
// テストデータ status-connected-in-call (画面共有の枠に載っていて、フレームが届いている人がいる) を使う。
//   (1) 平常の 1 行が、規則で数えた人数の要約になり、画面に表示されている 1 行目も同じになる。
//   (2) 画面の枠の人のエンコードの段階を unavailable にする: 上の規則も平常の 1 行も、その人を数えず、
//       行は「映像を表示できません」と表示する。
// ストリームの経路の表 (StreamRouter) は組み込まないので、人の値はテストデータのとおりである。
void run_summary(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("summary");
	SimPoll sim(ui_preview::fixture_by_id("status-connected-in-call"), options.style);
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());

	std::string screen_user;
	for (const ledger::PersonFact &p : sim.obs().people) {
		if (p.slot.has_value() && p.sharing) {
			screen_user = p.user_id;
			break;
		}
	}
	results.expect_true(group, QStringLiteral("テストデータ: 画面共有の枠に載っていて、画面共有している人がいる"),
			    !screen_user.empty(), QString::fromStdString(screen_user));
	if (screen_user.empty())
		return;

	auto row_of = [&](const std::string &user) -> dock::PersonRowWidget * {
		dock::PeopleList *list = sim.people();
		return list != nullptr ? list->row_for(user) : nullptr;
	};
	auto shown_before = std::make_shared<int>(-1);

	sim.start();
	Script s;
	s.then(1800, [&, shown_before] {
		std::vector<std::string> who;
		const int expect_shown = shown_by_rule(sim.obs(), &who);
		*shown_before = expect_shown;
		const bool screen_counted = std::find(who.begin(), who.end(), screen_user) != who.end();
		results.expect_true(group, QStringLiteral("(1) 画面の枠の人を「表示中」に数える (規則で数える)"), screen_counted,
				    QStringLiteral("規則で数えた人 %1").arg(static_cast<int>(who.size())));
		const QString expect_line = summary_text(sim.obs(), expect_shown);
		const QString line = summary_of_view(sim.view());
		results.add(group, QStringLiteral("(1) 平常の 1 行 (メッセージがないときの 1 行目) が、規則で数えた人数の要約"), line,
			    expect_line, line == expect_line ? Verdict::Pass : Verdict::Fail);
		const QString shown = first_line_full_text(sim.widget());
		results.add(group, QStringLiteral("(1) 画面に表示されている 1 行目も同じ"), shown, expect_line,
			    shown == expect_line ? Verdict::Pass : Verdict::Fail);
	});
	// (2) 画面の枠の人のエンコードの段階を unavailable にする (フレームの時刻は、届いたときのまま新しい)。
	// 上の規則はその人を数えず、平常の 1 行の「表示中」は 1 人減り、その人の行は「映像を表示できません」と
	// 表示する。ほかのテストデータには段階が unavailable の人がいないので、規則の段階の条件はここでだけ通る
	// (規則が段階を見ていなければ、(2) の最初の確認が不合格になる)。
	s.then(300, [&] {
		for (ledger::PersonFact &p : sim.obs().people) {
			if (p.user_id == screen_user)
				p.stage = std::string(ledger::kStageUnavailable);
		}
		sim.changed();
		sim.tick_soon();
	});
	s.then(900, [&, shown_before] {
		std::vector<std::string> who;
		const int expect_shown = shown_by_rule(sim.obs(), &who);
		const bool screen_counted = std::find(who.begin(), who.end(), screen_user) != who.end();
		const ledger::PersonFact *sp = nullptr;
		for (const ledger::PersonFact &p : sim.obs().people) {
			if (p.user_id == screen_user)
				sp = &p;
		}
		const bool frames_fresh = sp != nullptr && sp->last_video_frame_at_us.has_value() &&
					  sim.obs().now_us - *sp->last_video_frame_at_us < sim.obs().video_stall_threshold_us;
		results.expect_true(group, QStringLiteral("(2) エンコードの段階が unavailable の人は、フレームが届いていても規則で数えない"),
				    frames_fresh && !screen_counted && expect_shown == *shown_before - 1,
				    QStringLiteral("フレームが新しい %1、規則で数えた人 %2 (前は %3)")
					    .arg(frames_fresh ? 1 : 0)
					    .arg(expect_shown)
					    .arg(*shown_before));
		const QString expect_line = summary_text(sim.obs(), expect_shown);
		const QString line = summary_of_view(sim.view());
		results.add(group, QStringLiteral("(2) エンコードの段階が unavailable の人がいるときの平常の 1 行 (その人を数えない人数の要約)"), line,
			    expect_line, line == expect_line ? Verdict::Pass : Verdict::Fail);
		// 行の状態のテキストは、幅によって表示されたり隠れたりする (狭いとアイコンだけになる) ので、
		// 表示されているアイコンかテキストがあり、行のアクセシブルな説明 (アイコンのツールチップと同じ文) が
		// 状態のテキストを持つことを見る。
		const QString full = dock::ui::resolve_key(keys::kDockRowCantShowStatus);
		dock::PersonRowWidget *row = row_of(screen_user);
		const QString shown = row_status_shown(row);
		const RowTexts t = row_texts_contain(row, full);
		results.add(group, QStringLiteral("(2) 同じ時点で、その人の行が「映像を表示できません」と表示する (行と 1 行目が食い違わない)"),
			    shown + QStringLiteral(" desc=%1 tip=%2").arg(t.desc ? 1 : 0).arg(t.tip ? 1 : 0),
			    QStringLiteral("mark か text と desc=1"),
			    row != nullptr && !shown.isEmpty() && t.desc ? Verdict::Pass : Verdict::Fail,
			    QStringLiteral("状態のテキスト「%1」").arg(full));
	});
	const bool finished = s.run(30000);
	results.expect_true(group, QStringLiteral("手順が時間内に終わった"), finished);
	sim.stop();
	sim.dock()->hide();
}

} // namespace cordmux::dock_drive
