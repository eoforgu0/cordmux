// SPDX-License-Identifier: Apache-2.0

// layout と dispatch のシナリオ。layout は、静止した画像では確認できないドックの配置の動きを確認する。確認するのは、
// 状態タブのメッセージの並び、カードの高さの変化と、ドックの幅、フローティングのドックをメインウィンドウに戻したことで一覧が
// 動いた直後のクリックと、メッセージのカードのラベルの高さが 0 になったときの余白の計算である。
// dispatch は、並びの保留の間にためた状態タブの変化が、ポインターがスクロール領域の外へ移るときの Enter と Leave のイベントの
// 最中に反映されても、カードのボタンをその場で削除しないことを確認する。確認することの詳細は、ブロックごとのコメントにある。

#include "scenarios.h"

#include <QApplication>
#include <QDockWidget>
#include <QFrame>
#include <QMainWindow>
#include <QGuiApplication>
#include <QLabel>
#include <QMargins>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dock/dock_widget.h"
#include "dock/notice_list.h"
#include "dock/people_list.h"
#include "dock/person_row.h"
#include "dock/ui/wrap_label.h"
#include "drive_env.h"
#include "input.h"
#include "obs_dock.h"
#include "query.h"
#include "scen_common.h"
#include "sim_poll.h"

namespace cordmux::dock_drive {

namespace {

int notice_card_y(dock::DockWidget *widget, const std::string &key)
{
	for (QWidget *c : widget->findChildren<QWidget *>()) {
		auto *card = dynamic_cast<dock::NoticeCard *>(c);
		if (card != nullptr && card->notice().key == key && card->isVisibleTo(widget))
			return card->mapTo(widget, QPoint(0, 0)).y();
	}
	return -1;
}

} // namespace

void run_layout(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("layout");
	// ---- (2) メッセージの並び (メッセージのレベルの順。メッセージのレベルが同じなら表示した順) ----
	// 時刻を変えてメッセージを表示したり消したりしたとき、表示した順に並び、メッセージのレベルが同じメッセージを後から
	// 表示しても、前のメッセージの位置が動かないことを確認する。
	{
		SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 900);
		sim.dock()->show();
		sim.widget()->set_tab(config::DockTab::Status);
		sim.start();
		pump(800);
		int order_wrong = 0;
		int moved_earlier = 0;
		QStringList steps;
		std::map<std::string, int> y_before;
		auto check = [&](const QString &step) {
			const QStringList shown = shown_notice_keys(sim.widget());
			const QStringList expected = expected_notice_keys(sim.view());
			// View の並び (ledger::stabilize が、メッセージのレベルの順と表示した順に並べたもの) と、
			// 画面の並びが同じか。
			if (shown != expected)
				++order_wrong;
			// メッセージのレベルの順 (1、2、3) と、メッセージのレベルが同じメッセージの中の、表示した時刻の順。
			int prev_tier = 0;
			std::int64_t prev_at = 0;
			for (const ledger::dockview::Notice &n : sim.view().notices) {
				const int tier = n.tier == ledger::dockview::Tier::L1 ? 1 : n.tier == ledger::dockview::Tier::L2 ? 2 : 3;
				if (tier < prev_tier || (tier == prev_tier && n.appeared_at_us < prev_at))
					++order_wrong;
				prev_tier = tier;
				prev_at = n.appeared_at_us;
			}
			// 前からあるメッセージの位置: メッセージのレベルが同じか下のメッセージを新しく表示しただけなら動かない。
			for (const auto &[key, y] : y_before) {
				const int now_y = notice_card_y(sim.widget(), key);
				if (now_y >= 0 && y >= 0 && now_y != y)
					moved_earlier += 1;
			}
			steps << step + QStringLiteral(":") + shown.join(QLatin1Char(','));
			y_before.clear();
			for (const QString &k : shown)
				y_before[k.toStdString()] = notice_card_y(sim.widget(), k.toStdString());
		};
		// メッセージのレベル 2 を 2 つ、時刻をずらして表示する (後のものは前のものの下へ)。
		// メッセージのレベル 3 を加える (メッセージのレベル 2 の下へ)。
		sim.obs().versions.discord_side = "0.0.9"; // VersionRestartVesktop
		sim.changed();
		pump(2800);
		check(QStringLiteral("level2-first"));
		// DoubleAudio。ソースの名前がないと、ドックはメッセージを表示しない (ledger/rules_dock.cpp の DoubleAudio)。
		sim.obs().choices.discord_audio_source = ledger::DiscordAudioSource::Found;
		sim.obs().choices.discord_audio_source_name = "アプリケーション音声キャプチャ (ベータ版)";
		sim.changed();
		pump(2800);
		check(QStringLiteral("level2-second"));
		results.expect_true(group, QStringLiteral("(2) 2 つ目のメッセージのレベル 2 (DoubleAudio) が表示された"),
				    shown_notice_keys(sim.widget()).contains(QStringLiteral("DoubleAudio")));
		sim.obs().seen.discarded_with_marks = 2; // SeenDiscarded
		sim.changed();
		pump(2800);
		check(QStringLiteral("level3"));
		// 前のものを消す (後のものは上へ詰まる。消えた後は、位置の比較をやり直す)。
		sim.obs().versions.discord_side = "0.1.0+dev";
		sim.changed();
		pump(1800);
		y_before.clear();
		check(QStringLiteral("-level2-first"));
		results.expect_eq(group, QStringLiteral("(2) メッセージの並びが、メッセージのレベルの順と表示した順でなかった回数"), order_wrong, 0,
				  steps.join(QLatin1Char(' ')));
		results.expect_eq(group, QStringLiteral("(2) メッセージのレベルが同じか下のメッセージを表示したときに、前からあるメッセージが動いた回数"), moved_earlier, 0);
		sim.stop();
		sim.dock()->hide();
	}

	// ---- (6) アクティブでないドックの行を押したとき、フォーカスを戻すスクロールで別の人に適用されないか ----
	// ドックのウィンドウがアクティブでないときに行を押すと、そのクリックでウィンドウがアクティブになり、
	// Qt はウィンドウの中で最後にフォーカスのあったウィジェットへフォーカスを戻す。そのウィジェットがスクロール
	// 領域の表示範囲の外にあると、フォーカスに合わせたスクロールが、クリックが届く前に一覧を動かす。
	// 押した瞬間にポインターの下にあった行の人以外に適用されないことを確認する。
	// 手順: いちばん下の人の [...] にフォーカスを置く (クリックやキーボードで、行のウィジェットにフォーカスが残る
	// 状態)。ホイールで一覧のいちばん上までスクロールする (フォーカスのあるウィジェットは表示範囲の外へ)。
	// OBS のメインウィンドウを押す (Discord などほかのウィンドウへ移るのと同じく、ドックのウィンドウは
	// アクティブでなくなる)。表示されているいちばん上の人のチェックへポインターを動かし、700 ms 待ってから
	// (クリックを捨てるガードの時間の外で) 押す。
	{
		SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 320); // 一覧がスクロールする高さ (8 人が入りきらない)
		sim.dock()->show();
		Input input;
		input.activate(sim.dock());
		sim.start();
		pump(800);
		auto *scroll = sim.widget()->findChild<QScrollArea *>(QStringLiteral("cordmuxScroll"));
		std::string last_user;
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (row.checkable)
				last_user = row.user_id;
		}
		dock::PersonRowWidget *last_row = sim.people() != nullptr ? sim.people()->row_for(last_user) : nullptr;
		QWidget *last_dots = last_row != nullptr ? last_row->findChild<QWidget *>(QStringLiteral("cordmuxRowMenuButton")) : nullptr;
		if (last_dots != nullptr)
			last_dots->setFocus(Qt::TabFocusReason);
		pump(200);
		const int scroll_focused = scroll != nullptr ? scroll->verticalScrollBar()->value() : -1;
		const int scroll_max = scroll != nullptr ? scroll->verticalScrollBar()->maximum() : -1;
		// ホイールでいちばん上までスクロールする (フォーカスは残る)。
		input.move_to(global_rect(scroll != nullptr ? scroll->viewport() : sim.widget()).center());
		pump(100);
		for (int i = 0; i < 20 && scroll != nullptr && scroll->verticalScrollBar()->value() > 0; ++i) {
			input.wheel(120);
			pump(30);
		}
		const int scroll_top = scroll != nullptr ? scroll->verticalScrollBar()->value() : -1;
		const bool focus_kept = QApplication::focusWidget() == last_dots;
		// メインウィンドウを押す (ドックのウィンドウはアクティブでなくなる)。
		input.move_to(QPoint(600, 500));
		input.click();
		pump(300);
		const bool dock_inactive = QGuiApplication::focusWindow() != sim.dock()->windowHandle();
		// 表示されているいちばん上の、押せるチェックを持つ人。
		std::string aim_user;
		QRect aim_rect;
		const QRect vp = scroll != nullptr ? global_rect(scroll->viewport()) : QRect();
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (!row.checkable || !row.check_enabled || !aim_user.empty())
				continue;
			const QRect r = check_hit_rect_global(sim.people(), row.user_id).intersected(vp);
			if (r.height() >= 10) {
				aim_user = row.user_id;
				aim_rect = r;
			}
		}
		input.move_to(aim_rect.center());
		pump(700);
		const std::string under_before = user_of(Input::widget_at(aim_rect.center()));
		const std::size_t before = sim.actions().size();
		input.click();
		pump(200);
		const int scroll_after = scroll != nullptr ? scroll->verticalScrollBar()->value() : -1;
		std::string acted;
		for (std::size_t i = before; i < sim.actions().size(); ++i) {
			if (sim.actions()[i].ran && !sim.actions()[i].args.user_id.empty())
				acted = sim.actions()[i].args.user_id;
		}
		results.info(group, QStringLiteral("(6) 前提: フォーカスを残した人 / スクロールの位置 (フォーカスを置いたとき -> 押す前 -> 押した後、最大) / 押す前にドックのウィンドウがアクティブでない"),
			     QStringLiteral("%1 / %2 -> %3 -> %4、%5 / %6")
				     .arg(QString::fromStdString(last_user).right(2))
				     .arg(scroll_focused)
				     .arg(scroll_top)
				     .arg(scroll_after)
				     .arg(scroll_max)
				     .arg(dock_inactive ? QStringLiteral("yes") : QStringLiteral("no")),
			     QStringLiteral("フォーカスが残っていた: %1").arg(focus_kept ? QStringLiteral("yes") : QStringLiteral("no")));
		results.expect_true(group, QStringLiteral("(6) アクティブでないドックの行を押したとき、押した瞬間にポインターの下にあった行の人以外に適用されない"),
				    acted.empty() || acted == under_before,
				    QStringLiteral("ポインターの下 %1、動作した相手 %2")
					    .arg(QString::fromStdString(under_before).right(2),
						 acted.empty() ? QStringLiteral("(なし)") : QString::fromStdString(acted).right(2)));
		sim.stop();
		sim.dock()->hide();
	}

	// ---- (7) ドックの幅だけが変わった直後のクリック ----
	// 利用者がドックの境界 (フローティングならウィンドウの枠) をドラッグして幅を変え、放してすぐに行のチェックを
	// 押す。行は縦に動かない (チェックは行の左にあり、幅で位置が変わらない) ので、押した位置の下の人は変わらない。
	// 確認すること: クリックが別の人に適用されないこと (合否)、幅を変えて 100 ms 後のクリックが動作したか
	// (記録だけ。動作しないのは安全な側で、利用者が気付ける形かは実機テストで確認する)、700 ms 後のクリックが動作すること
	// (ガードが残り続けない。合否)。
	{
		SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 700);
		sim.dock()->show();
		Input input;
		input.activate(sim.dock());
		sim.start();
		pump(800);
		input.move_to(QPoint(900, 500)); // ドックの外 (メインウィンドウの上)
		pump(200);
		std::string target;
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (row.checkable && row.check_enabled && target.empty())
				target = row.user_id;
		}
		const QRect before_rect = check_hit_rect_global(sim.people(), target);
		sim.dock()->resize(340, 700); // 幅だけを変える (左上はそのまま)
		pump(20);
		const QRect after_rect = check_hit_rect_global(sim.people(), target);
		auto press_at_target = [&](int wait_ms, std::string *under, std::string *acted) {
			const QRect r = check_hit_rect_global(sim.people(), target);
			input.move_to(r.center());
			pump(wait_ms);
			*under = user_of(Input::widget_at(r.center()));
			const std::size_t before = sim.actions().size();
			input.click();
			pump(100);
			acted->clear();
			for (std::size_t i = before; i < sim.actions().size(); ++i) {
				if (sim.actions()[i].ran && !sim.actions()[i].args.user_id.empty())
					*acted = sim.actions()[i].args.user_id;
			}
		};
		std::string under_early, acted_early, under_late, acted_late;
		press_at_target(60, &under_early, &acted_early);
		if (!acted_early.empty()) {
			// 動作した分を戻す (同じ人をもう一度押す。ダブルクリックにならないように間を空ける)。
			pump(600);
			input.click();
			pump(100);
		}
		input.move_to(QPoint(900, 500));
		pump(200);
		press_at_target(700, &under_late, &acted_late);
		results.info(group, QStringLiteral("(7) 前提: 幅 420 -> 340 px でチェックの矩形が動いたか / 押した人"),
			     QStringLiteral("%1 / %2")
				     .arg(before_rect.topLeft() == after_rect.topLeft() ? QStringLiteral("動かない") : QStringLiteral("動いた"))
				     .arg(QString::fromStdString(target).right(2)));
		results.info(group, QStringLiteral("(7) 幅だけを変えて約 80 ms 後のクリックが動作したか (記録。動作しないのは安全な側)"),
			     acted_early.empty() ? QStringLiteral("no") : QStringLiteral("yes"),
			     QStringLiteral("行のガードは矩形の大きさも比べる。利用者が気付ける形かは実機テストで確認する"));
		results.expect_true(group, QStringLiteral("(7) 幅を変えた直後のクリックが、ポインターの下の人以外に適用されない"),
				    acted_early.empty() || acted_early == under_early,
				    QStringLiteral("ポインターの下 %1、動作した相手 %2")
					    .arg(QString::fromStdString(under_early).right(2),
						 acted_early.empty() ? QStringLiteral("(なし)") : QString::fromStdString(acted_early).right(2)));
		results.expect_true(group, QStringLiteral("(7) 幅を変えて 700 ms 後のクリックは、ポインターの下の人に適用される (ガードが残らない)"),
				    !acted_late.empty() && acted_late == under_late,
				    QStringLiteral("ポインターの下 %1、動作した相手 %2")
					    .arg(QString::fromStdString(under_late).right(2),
						 acted_late.empty() ? QStringLiteral("(なし)") : QString::fromStdString(acted_late).right(2)));
		sim.stop();
		sim.dock()->hide();
	}

	// ---- (8) フローティングのドックをメインウィンドウに戻した後のクリック ----
	// フローティングのドックをメインウィンドウのドックの場所へ戻す (QDockWidget::setFloating(false)。
	// タイトルバーのダブルクリックと同じ関数) と、行のスクリーン座標がすべて変わり、ガードが比べるウィンドウが、
	// フローティングのウィンドウからメインウィンドウに替わる。確認すること: 戻して 100 ms 後のクリックが別の人に
	// 動作しないこと (合否。動作したかは記録)、700 ms 後のクリックがポインターの下の人に適用されること (合否)、
	// 戻した後にメインウィンドウの中でドックが縦に動いた 40 ms 後のクリックが動作しないこと
	// (ガードが新しいウィンドウの中の動きを見ている。合否)。Windows のウィンドウの管理によるアクティブの移り方は
	// 再現したものなので、実機テストで 1 回確認する。
	{
		QWidget *main = main_window();
		const QRect main_geometry = main != nullptr ? main->geometry() : QRect();
		SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 700);
		sim.dock()->show();
		Input input;
		input.activate(sim.dock());
		sim.start();
		pump(800);
		input.move_to(QPoint(600, 500)); // メインウィンドウの上 (戻した後のドックの場所の外)
		pump(200);
		std::string target;
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (row.checkable && row.check_enabled && target.empty())
				target = row.user_id;
		}
		sim.dock()->setFloating(false);
		pump(20);
		const bool docked = !sim.dock()->isFloating() && sim.dock()->isVisible();
		auto press_at = [&](const QPoint &at, int wait_ms, std::string *under, std::string *acted) {
			input.move_to(at);
			pump(wait_ms);
			*under = user_of(Input::widget_at(at));
			const std::size_t before = sim.actions().size();
			input.click();
			pump(100);
			acted->clear();
			for (std::size_t i = before; i < sim.actions().size(); ++i) {
				if (sim.actions()[i].ran && !sim.actions()[i].args.user_id.empty())
					*acted = sim.actions()[i].args.user_id;
			}
		};
		std::string under_early, acted_early, under_late, acted_late;
		press_at(check_hit_rect_global(sim.people(), target).center(), 60, &under_early, &acted_early);
		if (!acted_early.empty()) {
			pump(600);
			input.click();
			pump(100);
		}
		input.move_to(QPoint(600, 500));
		pump(200);
		press_at(check_hit_rect_global(sim.people(), target).center(), 700, &under_late, &acted_late);
		if (!acted_late.empty()) {
			pump(600);
			input.click();
			pump(100);
		}
		// メインウィンドウの中でドックを下へ動かす。同じ列のドックの上に別のドックを置き、その高さを
		// 60 px 伸ばす (OBS で、上のドックとの境界が動いたのと同じ)。押す位置は、動いた後に押せる人
		// A のチェックがあり、動く前は別の人 B のチェックだった位置 (利用者は B を狙っていて、A が入ってきた)。
		// メインウィンドウごと動くときは、ガードの対象にしない (dock/people_list.cpp の note_positions は
		// ウィンドウの座標で比べる。利用者がウィンドウをドラッグしている間は、ポインターはタイトルバーにあり、
		// 行の上にない)。
		auto *main_window_ = qobject_cast<QMainWindow *>(main);
		std::unique_ptr<QDockWidget> other;
		if (main_window_ != nullptr) {
			other = std::make_unique<QDockWidget>(main_window_);
			other->setObjectName(QStringLiteral("driveOtherDock"));
			other->setWidget(new QWidget());
			main_window_->addDockWidget(Qt::RightDockWidgetArea, other.get());
			main_window_->splitDockWidget(other.get(), sim.dock(), Qt::Vertical);
			main_window_->resizeDocks({other.get(), sim.dock()}, {120, 600}, Qt::Vertical);
		}
		input.move_to(QPoint(600, 500));
		pump(800);
		// 動く前の押せるチェックの矩形 (スクリーン座標)。
		std::map<std::string, QRect> before_rects;
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (row.checkable && row.check_enabled)
				before_rects[row.user_id] = check_hit_rect_global(sim.people(), row.user_id);
		}
		const int dock_y0 = sim.dock()->mapTo(main, QPoint(0, 0)).y();
		if (main_window_ != nullptr)
			main_window_->resizeDocks({other.get()}, {other->height() + 60}, Qt::Vertical);
		pump(40);
		const int dock_y1 = sim.dock()->mapTo(main, QPoint(0, 0)).y();
		// 押す位置: 動いた後に押せるチェックを持つ人 A の矩形の中で、動く前は別の人 B の矩形だった位置
		// ((4) と同じ探し方)。
		QPoint aim;
		std::string aim_user;
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (!row.checkable || !row.check_enabled || !aim.isNull())
				continue;
			const QRect now_rect = check_hit_rect_global(sim.people(), row.user_id);
			for (int y = now_rect.top() + 2; y < now_rect.bottom() - 1 && aim.isNull(); y += 2) {
				const QPoint p(now_rect.left() + 10, y);
				for (const auto &[u, r] : before_rects) {
					if (u != row.user_id && r.contains(p)) {
						aim = p;
						aim_user = u;
						break;
					}
				}
			}
		}
		std::string under_moved, acted_moved;
		if (aim.isNull()) {
			results.add(group, QStringLiteral("(8) 前提: ドックが動いた後に別の人が来る位置がある"), QStringLiteral("no"), QStringLiteral("yes"),
				    Verdict::Fail, QStringLiteral("動いた量 %1 px").arg(dock_y1 - dock_y0));
			aim = QPoint(600, 500);
		}
		press_at(aim, 0, &under_moved, &acted_moved);
		// 対照: 同じ位置で 700 ms 待ってもう一度押すと動作する。上の「動作しない」がガードによるもので、
		// 位置が押せる所から外れたためではないことを確認する。
		std::string under_control, acted_control;
		pump(600);
		press_at(aim, 700, &under_control, &acted_control);
		results.info(group, QStringLiteral("(8) 前提: メインウィンドウのドックに戻った / 上のドックが伸びてドックが下へ動いた量 (px) / 狙った人 -> 動いた後にその位置にいる人 / 対照のクリックが動作した"),
			     QStringLiteral("%1 / %2 / %3 -> %4 / %5")
				     .arg(docked ? QStringLiteral("yes") : QStringLiteral("no"))
				     .arg(dock_y1 - dock_y0)
				     .arg(QString::fromStdString(aim_user).right(2), QString::fromStdString(under_moved).right(2))
				     .arg(acted_control.empty() ? QStringLiteral("no") : QStringLiteral("yes")));
		results.info(group, QStringLiteral("(8) メインウィンドウに戻して約 80 ms 後のクリックが動作したか (記録。動作しないのは安全な側)"),
			     acted_early.empty() ? QStringLiteral("no") : QStringLiteral("yes"),
			     QStringLiteral("戻した直後は、すべての行にガードが掛かる。利用者が気付ける形かは実機テストで確認する"));
		results.expect_true(group, QStringLiteral("(8) メインウィンドウに戻した直後のクリックが、ポインターの下の人以外に適用されない"),
				    acted_early.empty() || acted_early == under_early,
				    QStringLiteral("ポインターの下 %1、動作した相手 %2")
					    .arg(QString::fromStdString(under_early).right(2),
						 acted_early.empty() ? QStringLiteral("(なし)") : QString::fromStdString(acted_early).right(2)));
		results.expect_true(group, QStringLiteral("(8) メインウィンドウに戻して 700 ms 後のクリックは、ポインターの下の人に適用される"),
				    !acted_late.empty() && acted_late == under_late,
				    QStringLiteral("ポインターの下 %1、動作した相手 %2")
					    .arg(QString::fromStdString(under_late).right(2),
						 acted_late.empty() ? QStringLiteral("(なし)") : QString::fromStdString(acted_late).right(2)));
		results.expect_true(group, QStringLiteral("(8) メインウィンドウに戻した後、上のドックが伸びてドックが動いた 40 ms 後のクリックが動作しない (ガードが新しい祖先を見る)"),
				    acted_moved.empty(),
				    acted_moved.empty() ? QString()
							: QStringLiteral("動作した (ポインターの下 %1、相手 %2)")
								  .arg(QString::fromStdString(under_moved).right(2),
								       QString::fromStdString(acted_moved).right(2)));
		sim.stop();
		if (main_window_ != nullptr && other)
			main_window_->removeDockWidget(other.get());
		other.reset();
		if (main != nullptr)
			main->setGeometry(main_geometry);
		sim.dock()->hide();
	}

	// ---- (5) メッセージが消えてまた表示されたときの並び (mistap の (e) で見つかった食い違いの再現) ----
	// メッセージのレベル 1 (空き容量がなくなって録画が止まった)、メッセージのレベル 2 (バージョンの食い違い、二重の音、
	// cordmux のソースがない)、メッセージのレベル 3 (過去の参加者を消した) を表示し、メッセージのレベル 2 の
	// 「cordmux のソースがない」を消してまた表示する。(A) ポインターが外にあるとき、(B) ポインターがスクロール領域の上にある (並びの保留の) 間に消して
	// 表示し、外へ出たとき。どちらも、画面の並び (カードの上から順) が View の並び (メッセージのレベルの順と表示した順)
	// と同じになるかを確認する。
	{
		SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 900);
		sim.dock()->show();
		sim.widget()->set_tab(config::DockTab::Status);
		Input input;
		input.activate(sim.dock());
		sim.start();
		pump(500);
		input.move_to(QPoint(900, 500));
		ledger::Observation &obs = sim.obs();
		obs.recording.stopped_by_space_exhausted = true; // メッセージのレベル 1
		obs.versions.discord_side = "0.0.9";            // メッセージのレベル 2
		obs.seen.discarded_with_marks = 2; // メッセージのレベル 3
		obs.choices.discord_audio_source = ledger::DiscordAudioSource::Found; // メッセージのレベル 2 (二重の音)
		obs.choices.discord_audio_source_name = "Application Audio Capture";
		sim.changed();
		pump(2600);
		obs.choices.composite_present = false; // メッセージのレベル 2 (cordmux のソースがない。後から表示される)
		sim.changed();
		pump(2600);
		auto same_order = [&](const QString &label, QStringList *notes) {
			const QStringList shown = shown_notice_keys(sim.widget());
			const QStringList expected = expected_notice_keys(sim.view());
			*notes << label + QStringLiteral(": ") + shown.join(QLatin1Char(',')) +
					  (shown == expected ? QString() : QStringLiteral(" (View: ") + expected.join(QLatin1Char(',')) + QLatin1Char(')'));
			return shown == expected;
		};
		QStringList notes;
		int wrong = 0;
		wrong += same_order(QStringLiteral("start"), &notes) ? 0 : 1;
		// (A) 外にある間に消して、また表示する。
		obs.choices.composite_present = true;
		sim.changed();
		pump(1600);
		obs.choices.composite_present = false;
		sim.changed();
		pump(2600);
		wrong += same_order(QStringLiteral("A"), &notes) ? 0 : 1;
		// (B) スクロール領域の上にある (並びの保留の) 間に消して表示し、外へ出る。
		auto *scroll = sim.widget()->findChild<QScrollArea *>(QStringLiteral("cordmuxStatusScroll"));
		if (scroll != nullptr)
			input.move_to(scroll->viewport()->mapToGlobal(scroll->viewport()->rect().center()));
		pump(200);
		const bool held = sim.widget()->layout_held();
		obs.choices.composite_present = true;
		sim.changed();
		pump(1600);
		obs.choices.composite_present = false;
		sim.changed();
		pump(2600);
		input.move_to(QPoint(900, 500));
		pump(700);
		wrong += same_order(QStringLiteral("B (held=") + QString::number(held ? 1 : 0) + QLatin1Char(')'), &notes) ? 0 : 1;
		results.expect_eq(group, QStringLiteral("(5) メッセージが消えてまた表示された後、画面の並びが View の並び (メッセージのレベルの順と表示した順) と違った回数"),
				  wrong, 0, notes.join(QStringLiteral(" | ")));
		sim.stop();
		sim.dock()->hide();
	}

	// ---- (3) 録画ファイルの作成の進み具合の書き換えでカードの高さが変わる回数 (記録だけ) ----
	{
		SimPoll sim(ui_preview::fixture_by_id("notice-record-finalize-running"), options.style);
		sim.dock()->setGeometry(1300, 120, 300, 700);
		sim.dock()->show();
		sim.widget()->set_tab(config::DockTab::Status);
		sim.start();
		pump(2800);
		std::string key;
		for (const ledger::dockview::Notice &n : sim.view().notices) {
			if (n.rule_name == "RecordFinalizeRunning")
				key = n.key;
		}
		int height_changes = 0;
		int last_h = -1;
		for (int done = 0; done <= 18; ++done) {
			sim.obs().recording.finalize_files_total = 18;
			sim.obs().recording.finalize_files_done = done;
			sim.obs().recording.finalize_current_index = done < 18 ? done + 1 : 0;
			sim.obs().recording.finalize_permille = (done * 137) % 1000;
			sim.changed();
			pump(600);
			int h = -1;
			for (QWidget *c : sim.widget()->findChildren<QWidget *>()) {
				auto *card = dynamic_cast<dock::NoticeCard *>(c);
				if (card != nullptr && card->notice().key == key && card->isVisibleTo(sim.widget()))
					h = card->height();
			}
			if (last_h >= 0 && h >= 0 && h != last_h)
				++height_changes;
			if (h >= 0)
				last_h = h;
		}
		results.info(group, QStringLiteral("(3) 録画ファイルの作成の進み具合 (18 個のファイル) の書き換えでカードの高さが変わった回数 (幅 300 px)"),
			     QString::number(height_changes),
			     QStringLiteral("高さが変わると下の行が動く (並びの保留とクリックを捨てるガードが受け止める)"));
		sim.stop();
		sim.dock()->hide();
	}

	// ---- (9) メッセージのカードのラベルの高さが 0 になっても、余白の計算が終わるか ----
	// テーマの frame-notice の中のラベルは、テーマの余白 (Yami の .frame-notice QLabel の上下の padding) を
	// contentsMargins に持つ。表示しているラベルの余白を変えると、Qt はその場で resizeEvent を送り、WrapLabel は
	// resizeEvent の中で余白を計算し直す。高さが 0 のときに、計算した余白が余白を変えるたびに変わると、resizeEvent が
	// 終わらず、スタックオーバーフローで落ちる。上下に余白を持つ WrapLabel と、ドックのメッセージのカードのうち
	// frame-notice を使うもの (使うかはテーマで決まる。DockLook::card_frame_ok) のラベルを、高さ 0 にしてから元に戻し、
	// 落ちないことと、余白が元に戻ることを確認する。前者は、テーマによらず確認するため、テーマの代わりにスタイル
	// シートで上下の余白を付ける。落ちると dock_drive も終了する (結果の表は書かれず、落ちた場所が標準エラー出力に
	// 書かれる)。
	{
		say(QStringLiteral("layout (9): ここでクラッシュしたら、メッセージのラベルの余白の計算が resizeEvent の中で終わらない"));
		QFrame host;
		host.setStyleSheet(QStringLiteral("QLabel { padding: 4px 0px; }"));
		auto *host_layout = new QVBoxLayout(&host);
		auto *host_label = new dock::ui::WrapLabel(QStringLiteral("Vesktop の設定が、cordmux とは別の場所を指しています。"), &host);
		host_label->set_breaks(dock::ui::WrapBreaks::KeepNumbers);
		host_layout->addWidget(host_label);
		host.setGeometry(1300, 120, 360, 120);
		host.show();
		SimPoll sim(ui_preview::fixture_by_id("status-vencord-elsewhere"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 600);
		sim.dock()->show();
		sim.widget()->set_tab(config::DockTab::Status);
		sim.start();
		pump(2800); // 表示するまでの待ち (2 秒) の後
		auto in_frame_notice = [](const QWidget *w) {
			for (; w != nullptr; w = w->parentWidget()) {
				if (w->property("class").toString().split(QLatin1Char(' ')).contains(QStringLiteral("frame-notice")))
					return true;
			}
			return false;
		};
		std::vector<QPointer<dock::ui::WrapLabel>> labels{host_label};
		int in_cards = 0;
		for (QLabel *l : sim.widget()->findChildren<QLabel *>()) {
			auto *wrap = dynamic_cast<dock::ui::WrapLabel *>(l);
			if (wrap != nullptr && wrap->isVisible() && in_frame_notice(wrap)) {
				labels.emplace_back(wrap);
				++in_cards;
			}
		}
		results.expect_true(group, QStringLiteral("(9) 前提: スタイルシートで余白を付けたラベルが、上下の余白を持つ"),
				    host_label->contentsMargins().top() > 0 && host_label->contentsMargins().bottom() > 0,
				    QStringLiteral("上 %1 px / 下 %2 px")
					    .arg(host_label->contentsMargins().top())
					    .arg(host_label->contentsMargins().bottom()));
		results.info(group, QStringLiteral("(9) ドックのメッセージのカードのうち、frame-notice の中に表示しているラベルの数"),
			     QString::number(in_cards), QStringLiteral("0 なら、このテーマではカードが frame-notice を使わない"));
		int changed = 0;
		for (const QPointer<dock::ui::WrapLabel> &l : labels) {
			if (l.isNull())
				continue;
			const QSize size = l->size();
			const QMargins before = l->contentsMargins();
			l->resize(size.width(), 0);
			l->resize(size);
			if (l->contentsMargins() != before)
				++changed;
		}
		pump(300);
		results.expect_eq(group, QStringLiteral("(9) 高さを 0 にして戻した後に余白が変わったラベルの数"), changed, 0);
		sim.stop();
		sim.dock()->hide();
		host.hide();
	}

	// ドックの枠組みのシナリオ (scen_framework.cpp)。
	run_framework(options, results);
}

// ---- dispatch: Enter と Leave のイベントの最中に、カードのボタンを削除しないか ----
// mistap で見つかったクラッシュの形。製品がクラッシュすると dock_drive も終了する (結果の表は書かれず、
// クラッシュした場所が標準エラー出力に書かれる) ので、layout とは別のシナリオにして最後に実行する。
void run_dispatch(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("dispatch");
	// 手順: 状態タブに、ボタンのあるカード (cordmux の録画を開始できなかった RecordDirUnwritable) を表示する。ポインターをカードの
	// 上へ動かす (並びの保留)。原因を消して、消すまでの待ち (1 秒) より長く、止まってから反映するまでの時間より短く待つ。ポインターを
	// 1 回でタブのバーの上へ動かす。スクロール領域を出るイベントで並びの保留が解け、ためた変化 (カードをなくす) がその場で
	// 反映される。そのとき Qt は、まだ Enter と Leave を送るウィジェットの一覧を持っている。ボタンがこの移動の最中に削除されたかを
	// 数える。
	{
		say(QStringLiteral("dispatch: ここでクラッシュしたら、製品が Enter と Leave のイベントの最中にウィジェットを削除している"));
		SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 700);
		sim.dock()->show();
		sim.widget()->set_tab(config::DockTab::Status);
		Input input;
		input.activate(sim.dock());
		sim.start();
		pump(600);
		input.move_to(QPoint(900, 500)); // ドックの外
		sim.obs().seen.discarded_with_marks = 2;
		sim.obs().recording.save_dir_writable = false;
		sim.obs().recording.phase = ledger::RecordPhase::StartFailed;
		sim.changed();
		pump(2800);
		QPointer<QPushButton> old_button;
		for (QWidget *c : sim.widget()->findChildren<QWidget *>(QStringLiteral("cordmuxNoticeCard"))) {
			if (!c->isVisibleTo(sim.widget()))
				continue;
			for (QPushButton *b : c->findChildren<QPushButton *>()) {
				if (b->isVisibleTo(sim.widget()) && old_button.isNull())
					old_button = b;
			}
		}
		// カードの上へ動かす (並びの保留)。
		if (!old_button.isNull())
			input.move_to(old_button->mapToGlobal(old_button->rect().center()));
		pump(100);
		const bool held = sim.widget()->layout_held();
		sim.settings().layout_hold_idle_ms = 60000;
		sim.obs().recording.save_dir_writable = true;
		sim.obs().recording.phase = ledger::RecordPhase::Idle;
		sim.changed();
		pump(1500);
		const bool still_there = !old_button.isNull() && old_button->isVisible();
		bool moving = false;
		bool deleted_while_moving = false;
		if (!old_button.isNull()) {
			QObject::connect(old_button.data(), &QObject::destroyed, [&moving, &deleted_while_moving] {
				if (moving)
					deleted_while_moving = true;
			});
		}
		auto *tabs = sim.widget()->findChild<QWidget *>(QStringLiteral("cordmuxTabs"));
		moving = true;
		if (tabs != nullptr)
			input.move_to(tabs->mapToGlobal(tabs->rect().center()));
		moving = false;
		pump(300);
		results.info(group, QStringLiteral("前提: カードのボタンがあった / カードの上で並びを保留した / 原因を消した後もボタンが残っていた (並びの保留)"),
			     QStringLiteral("%1 / %2 / %3")
				     .arg(still_there || !old_button.isNull() ? QStringLiteral("yes") : QStringLiteral("no"))
				     .arg(held ? QStringLiteral("yes") : QStringLiteral("no"))
				     .arg(still_there ? QStringLiteral("yes") : QStringLiteral("no")));
		results.expect_true(group,
				    QStringLiteral("ポインターがスクロール領域からタブのバーへ移る Enter と Leave のイベントの最中に、カードのボタンを削除しない (削除すると Qt が"
						   "削除されたウィジェットへイベントを送る)"),
				    !deleted_while_moving,
				    deleted_while_moving ? QStringLiteral("Enter と Leave のイベントの最中に削除された") : QString());
		sim.stop();
		sim.dock()->hide();
	}
}

} // namespace cordmux::dock_drive
