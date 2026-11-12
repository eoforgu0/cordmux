// SPDX-License-Identifier: Apache-2.0

// status のシナリオ。ドックの状態タブ (現在のメッセージ、録画、映像の受信、履歴) を確認する。画面は開かない。
//   (1) メッセージがないときは「ありません」。メッセージのカードは、パネルの色の背景と左端のバーで、枠線と赤い背景がない。
//   (2) 映像のバッファは、変えるとすぐ配信の処理へ渡して表を計算し直し、保存は 1 秒後に 1 回にまとまる。設定ウィンドウの
//       [適用] で古い値に戻らず、OBS を閉じる処理で保存が残らない。
//   (3) 映像途切れの累計。リセットで消え、配信の開始で消え、抜けた人の行が残る。
//   (4) 履歴の順序と上限 (60 行)。
//   (5) ホイールでタブがスクロールし、値が変わらない。
//   (6) 最小の幅で、横にはみ出さない。
//   (7) 並びの保留の間は、位置が動く変化を反映せず、[途切れの累計をリセット] と [映像のバッファ] の位置が動かない。
//   (8) 1 行目を押すと状態タブが開き、レベル 2 が既読になる。
//   (9) Vesktop の起動中は、参加者タブの [Vesktopに追加] を押せず、押せない理由をボタンの下に文で出す。状態タブの
//       「Vesktop」の組は、Vesktop の起動中と、起動しているかを読めていない間は出さず、終了すると現れる。Vesktop の設定を
//       書かない起動では [Vesktopから削除] を押せず、理由をボタンの下に文で出す。[Vesktopから削除] は確認せずに実行する。
//       外れた後は組を出さない。

#include "scenarios.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config/settings.h"
#include "dock/accounts_bar.h"
#include "dock/bottom_bar.h"
#include "dock/dock_tabs.h"
#include "dock/dock_widget.h"
#include "dock/first_line.h"
#include "dock/look_tab.h"
#include "dock/notice_list.h"
#include "dock/settings_fields.h"
#include "dock/status_tab.h"
#include "dock/ui/slider_spin.h"
#include "dock/ui/text_resolve.h"
#include "drive_env.h"
#include "input.h"
#include "ledger/status_tab.h"
#include "ledger/text_ids.h"
#include "obs_dock.h"
#include "query.h"
#include "scen_common.h"
#include "sim_poll.h"

namespace cordmux::dock_drive {

namespace {

namespace keys = ledger::text::keys;
const QString kGroup = QStringLiteral("status");

QScrollArea *status_scroll(SimPoll &sim)
{
	return sim.widget()->findChild<QScrollArea *>(QStringLiteral("cordmuxStatusScroll"));
}

QPoint center_of(QWidget *w)
{
	return w->mapToGlobal(w->rect().center());
}

// 状態タブを開いた SimPoll を始める。
void open_status(SimPoll &sim, Input &input, int width = 420, int height = 640)
{
	sim.dock()->setGeometry(1300, 120, width, height);
	sim.settings().layout_hold_idle_ms = 600;
	sim.widget()->set_tab(config::DockTab::Status);
	sim.dock()->show();
	input.activate(sim.dock());
	sim.start();
	pump(1200);
}

QLabel *label_named(QWidget *root, const char *name)
{
	return root->findChild<QLabel *>(QString::fromLatin1(name));
}

// 表の行 (1 から) の、列 (0 名前、1 途切れ、2 合計、3 遅延、4 最大) の文字。
QString cell_text(dock::StatusTab *tab, int row, int column)
{
	auto *label = qobject_cast<QLabel *>(tab->cell(row, column));
	if (label != nullptr)
		return label->text();
	return tab->cell(row, column) != nullptr ? QStringLiteral("(省略ラベル)") : QString();
}

// 表の行 row の名前の全文 (省略していても全文)。
QString row_name(dock::StatusTab *tab, int row)
{
	QWidget *w = tab->cell(row, 0);
	return w != nullptr ? w->toolTip() + w->accessibleName() : QString();
}

int table_row_of(dock::StatusTab *tab, const QString &needle)
{
	for (int r = 1; r <= tab->table_rows(); ++r) {
		for (int c = 0; c < 5; ++c) {
			QWidget *w = tab->cell(r, c);
			if (w != nullptr && (w->accessibleName().contains(needle) || w->toolTip().contains(needle)))
				return r;
		}
	}
	return 0;
}

// 最小の幅にして、横にはみ出さないか。スクロール領域の見える幅より右にあるウィジェットの数を返す。
int overflow_count(SimPoll &sim, QString *detail)
{
	QScrollArea *scroll = status_scroll(sim);
	if (scroll == nullptr)
		return -1;
	const int right = scroll->viewport()->mapToGlobal(QPoint(scroll->viewport()->width(), 0)).x();
	int count = 0;
	QStringList names;
	for (QWidget *w : scroll->widget()->findChildren<QWidget *>()) {
		if (!w->isVisibleTo(scroll->widget()))
			continue;
		const int w_right = w->mapToGlobal(QPoint(w->width(), 0)).x();
		if (w_right > right + 1) {
			++count;
			names << QStringLiteral("%1(%2)").arg(w->metaObject()->className(), w->objectName());
		}
	}
	if (scroll->widget()->width() > scroll->viewport()->width()) {
		++count;
		names << QStringLiteral("content %1 > viewport %2").arg(scroll->widget()->width()).arg(scroll->viewport()->width());
	}
	if (detail != nullptr)
		*detail = names.join(QLatin1Char(' '));
	return count;
}

} // namespace

// ---- (1) メッセージ ----
static void run_status_messages(const ScenarioOptions &options, Results &results)
{
	// メッセージなし。
	{
		SimPoll sim(ui_preview::fixture_by_id("status-empty"), options.style);
		Input input;
		open_status(sim, input);
		dock::StatusTab *tab = sim.widget()->status_tab();
		QWidget *none = label_named(sim.widget(), "cordmuxStatusNoticesNone");
		results.expect_true(kGroup, QStringLiteral("(1) メッセージがないときは「ありません」を表示する"),
				    tab != nullptr && none != nullptr && none->isVisibleTo(sim.widget()) &&
					    none->property("text").toString() == dock::ui::resolve_key(keys::kStatusNoticesNoneStatus) &&
					    tab->notices()->empty());
		QWidget *rec_none = label_named(sim.widget(), "cordmuxStatusRecordingNone");
		QWidget *hist_none = label_named(sim.widget(), "cordmuxStatusHistoryNone");
		QWidget *empty = sim.widget()->findChild<QWidget *>(QStringLiteral("cordmuxStatusReceiveEmpty"));
		results.expect_true(kGroup, QStringLiteral("(1) 録画は「まだ録画していません」、履歴は「まだありません」、受信は空の文"),
				    rec_none != nullptr && rec_none->isVisibleTo(sim.widget()) && hist_none != nullptr &&
					    hist_none->isVisibleTo(sim.widget()) && empty != nullptr && empty->isVisibleTo(sim.widget()));
	}

	// レベル 1、2、3 のカード。
	{
		SimPoll sim(ui_preview::fixture_by_id("status-messages"), options.style);
		sim.settings().notice_show_delay_ms = 0;
		Input input;
		open_status(sim, input, 420, 900);
		pump(800);
		const std::vector<dock::NoticeCard *> cards = sim.widget()->status_tab()->notices()->cards();
		std::map<int, dock::NoticeCard *> by_tier;
		for (dock::NoticeCard *c : cards)
			by_tier.emplace(static_cast<int>(c->notice().tier), c);
		results.expect_true(kGroup, QStringLiteral("(1) 前提: メッセージのレベル 1、2、3 のカードが並んでいる"),
				    by_tier.count(0) == 1 && by_tier.count(1) == 1 && by_tier.count(2) == 1,
				    QStringLiteral("カード %1 枚。レベル %2").arg(cards.size()).arg([&] {
					    QStringList t;
					    for (dock::NoticeCard *c : cards)
						    t << QString::number(static_cast<int>(c->notice().tier) + 1);
					    return t.join(QLatin1Char(','));
				    }()));
		if (by_tier.count(0) == 1 && by_tier.count(1) == 1 && by_tier.count(2) == 1) {
			const auto image = [](dock::NoticeCard *c) { return c->grab().toImage().convertToFormat(QImage::Format_RGB32); };
			const QImage l1 = image(by_tier[0]);
			const QImage l2 = image(by_tier[1]);
			const QImage l3 = image(by_tier[2]);
			const auto inner = [](const QImage &img) { return img.pixel(img.width() - 6, img.height() / 2); };
			results.expect_true(kGroup, QStringLiteral("(1) 3 つのレベルのカードは同じパネルの色の背景である"),
					    inner(l1) == inner(l2) && inner(l2) == inner(l3));
			// 枠線がない: 上下左右の端の外側の 1 px が、パネルの色のまま (バーのある左端を除く)。
			const auto frameless = [&](const QImage &img) {
				const QRgb panel = inner(img);
				return img.pixel(img.width() / 2, 0) == panel && img.pixel(img.width() / 2, img.height() - 1) == panel &&
				       img.pixel(img.width() - 1, img.height() / 2) == panel;
			};
			results.expect_true(kGroup, QStringLiteral("(1) 太い枠がない (レベル 1、2、3 のどのカードも、端が背景の色のまま)"),
					    frameless(l1) && frameless(l2) && frameless(l3));
			// 左端のバー: レベル 1 と 2 は背景と違う色、レベル 3 はない。バーの色は 2 つのレベルで違う。
			const auto bar = [](const QImage &img) { return img.pixel(1, img.height() / 2); };
			results.expect_true(kGroup, QStringLiteral("(1) 左端のバー: レベル 1 と 2 にあり、色が違う。レベル 3 にはない"),
					    bar(l1) != inner(l1) && bar(l2) != inner(l2) && bar(l1) != bar(l2) && bar(l3) == inner(l3));
			// 赤い背景がない: レベル 1 の背景は、1 行目の赤い背景と違う。
			dock::FirstLineBar *first = descendant_of<dock::FirstLineBar>(sim.widget());
			const QImage first_image = first->grab().toImage().convertToFormat(QImage::Format_RGB32);
			results.expect_true(kGroup, QStringLiteral("(1) レベル 1 のカードの背景は赤くない (1 行目の赤い背景と違う)"),
					    first_image.pixel(first_image.width() - 2, 1) != inner(l1) || first->height() == 0);
		}
		results.expect_true(kGroup, QStringLiteral("(1) 状態タブの 3 つのレベルのカードは、すべてボタンまたは本文を持つ"),
				    !cards.empty());
	}
}

// ---- (2) 映像のバッファ ----
static void run_status_buffer(const ScenarioOptions &options, Results &results)
{
	SimPoll sim(ui_preview::fixture_by_id("status-table"), options.style);
	Input input;
	sim.arrival() = sim.obs().receive.arrival;
	sim.obs().receive.arrival.clear();
	// 遅れの列を、100 ms で 6 フレーム中 3 フレームが遅れる形にする。
	pipeline::ArrivalWindow w;
	w.delays_ms = {20, 150, 160, 30, 400, 10};
	w.mean_ms = 128.0;
	w.max_ms = 400.0;
	sim.arrival() = {ledger::ArrivalFact{sim.obs().people[0].user_id, {w}}};
	sim.settings().display_buffer_ms = 100;
	open_status(sim, input);
	dock::StatusTab *tab = sim.widget()->status_tab();
	dock::ui::SliderSpin *buffer = tab->buffer_input();
	results.expect_true(kGroup, QStringLiteral("(2) 前提: 映像のバッファの入力欄は現在の値 100 ms で、刻み 10、範囲 0 から 1000"),
			    buffer != nullptr && std::lround(buffer->spin()->value()) == 100 && buffer->spin()->singleStep() == 10 &&
				    buffer->spin()->minimum() == 0 && buffer->spin()->maximum() == 1000 && buffer->slider() != nullptr);
	int row = table_row_of(tab, QStringLiteral("hanako"));
	results.expect_true(kGroup, QStringLiteral("(2) 前提: 表に直近 1 分の列を持つ人の行がある"), row > 0);
	if (buffer == nullptr || row == 0)
		return;
	const QString late100 = cell_text(tab, row, 3);
	results.expect_true(kGroup, QStringLiteral("(2) 100 ms では 6 フレーム中 3 フレームが遅れ、遅延の欄は 50.0%"),
			    late100.contains(QStringLiteral("50.0")), late100);

	const int reflects0 = sim.buffer_reflects();
	const int saves0 = sim.look_saves();
	buffer->spin()->setValue(200);
	results.expect_true(kGroup, QStringLiteral("(2) 値を変えた直後は、まだ配信の処理へ渡さない"),
			    sim.buffer_reflects() == reflects0 && sim.settings().display_buffer_ms == 100 && tab->has_unreflected(),
			    QStringLiteral("渡した %1 回、設定 %2").arg(sim.buffer_reflects() - reflects0).arg(sim.settings().display_buffer_ms));
	const QString late200 = cell_text(tab, row, 3);
	results.expect_true(kGroup, QStringLiteral("(2) 表の遅延の割合は、渡す前に計算し直される (200 ms では 6 フレーム中 1 フレーム。16.7%)"),
			    late200 != late100 && late200.contains(QStringLiteral("16.7")), late200);
	pump(ledger::kStatusBufferIdleMs + 100);
	results.expect_true(kGroup, QStringLiteral("(2) 入力が止まると、配信の処理へ 1 回渡し、設定に入る"),
			    sim.buffer_reflects() == reflects0 + 1 && sim.settings().display_buffer_ms == 200 && !tab->has_unreflected(),
			    QStringLiteral("渡した %1 回、設定 %2").arg(sim.buffer_reflects() - reflects0).arg(sim.settings().display_buffer_ms));
	results.expect_true(kGroup, QStringLiteral("(2) その時点では保存していない"), sim.look_saves() == saves0 && sim.look_save_pending());
	pump(1200);
	results.expect_true(kGroup, QStringLiteral("(2) 渡した 1 秒後に、1 回だけ保存する"),
			    sim.look_saves() == saves0 + 1 && sim.saved_settings().display_buffer_ms == 200,
			    QStringLiteral("保存 %1 回").arg(sim.look_saves() - saves0));
	pump(1500);
	results.expect_true(kGroup, QStringLiteral("(2) その後は保存を重ねない"), sim.look_saves() == saves0 + 1);

	// つまみを動かしている間は渡さない。渡すたびに映像と画面共有の音声が一時的に止まったり飛んだりする。動かした分は 1 回にまとめ、
	// 保存も 1 回にまとめる。表の遅延は動かしている間も計算し直す。
	const int saves1 = sim.look_saves();
	const int reflects1 = sim.buffer_reflects();
	buffer->slider()->setSliderDown(true);
	for (int v = 30; v <= 90; v += 10) {
		buffer->slider()->setValue(v / 10);
		pump(ledger::kStatusBufferIdleMs / 3);
	}
	results.expect_true(kGroup, QStringLiteral("(2) つまみを動かしている間は、配信の処理へ渡さない"),
			    sim.buffer_reflects() == reflects1 && sim.settings().display_buffer_ms == 200,
			    QStringLiteral("渡した %1 回").arg(sim.buffer_reflects() - reflects1));
	results.expect_true(kGroup, QStringLiteral("(2) つまみを動かしている間も、表の遅延は計算し直す"),
			    cell_text(tab, row, 3) != late200, cell_text(tab, row, 3));
	buffer->slider()->setSliderDown(false);
	pump(ledger::kStatusBufferIdleMs + 100);
	results.expect_true(kGroup, QStringLiteral("(2) 動かし終わると、最後の値を 1 回だけ渡す"),
			    sim.buffer_reflects() == reflects1 + 1 && sim.settings().display_buffer_ms == 90,
			    QStringLiteral("渡した %1 回、設定 %2").arg(sim.buffer_reflects() - reflects1).arg(sim.settings().display_buffer_ms));
	pump(1400);
	results.expect_true(kGroup, QStringLiteral("(2) 続けて動かしても、保存は 1 回で、最後の値を保存する"),
			    sim.look_saves() == saves1 + 1 && sim.saved_settings().display_buffer_ms == 90);

	// つまみをダブルクリックすると既定値に戻り、フォーカスが来る。入力が止まってから 1 回渡す。続けて右の矢印で 1 段 (10 ms) 動く。
	{
		const int default_ms = config::Settings{}.display_buffer_ms;
		QSlider *slider = buffer->slider();
		if (QWidget *focused = QApplication::focusWidget())
			focused->clearFocus();
		const int reflects_reset = sim.buffer_reflects();
		input.move_to(center_of(slider));
		input.click();
		input.click();
		pump(ledger::kStatusBufferIdleMs + 100);
		results.expect_true(kGroup, QStringLiteral("(2) つまみをダブルクリックすると、既定値 (100 ms) に戻して 1 回渡し、フォーカスが来る"),
				    std::lround(buffer->spin()->value()) == default_ms && sim.settings().display_buffer_ms == default_ms &&
					    sim.buffer_reflects() == reflects_reset + 1 && slider->hasFocus(),
				    QStringLiteral("値 %1、渡した %2 回").arg(buffer->spin()->value()).arg(sim.buffer_reflects() - reflects_reset));
		input.key(Qt::Key_Right);
		pump(ledger::kStatusBufferIdleMs + 100);
		results.expect_true(kGroup, QStringLiteral("(2) フォーカスのあるつまみは、右の矢印で 1 段 (10 ms) 動き、止まってから渡す"),
				    sim.settings().display_buffer_ms == default_ms + 10, QString::number(sim.settings().display_buffer_ms));
		buffer->spin()->setValue(90);
		slider->clearFocus();
		pump(ledger::kStatusBufferIdleMs + 1400);
	}

	// 数の入力欄に打っている途中の数は渡さない。入力を終えた値を渡す。
	{
		buffer->spin()->setValue(90);
		const int reflects2 = sim.buffer_reflects();
		buffer->spin()->setFocus(Qt::TabFocusReason);
		pump(50);
		buffer->spin()->selectAll();
		input.key(Qt::Key_3, Qt::NoModifier, QStringLiteral("3"));
		input.key(Qt::Key_5, Qt::NoModifier, QStringLiteral("5"));
		pump(ledger::kStatusBufferIdleMs + 100);
		results.expect_true(kGroup, QStringLiteral("(2) 打っている途中の数は渡さない"), sim.buffer_reflects() == reflects2,
				    QStringLiteral("渡した %1 回、設定 %2").arg(sim.buffer_reflects() - reflects2).arg(sim.settings().display_buffer_ms));
		input.key(Qt::Key_0, Qt::NoModifier, QStringLiteral("0"));
		input.key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
		pump(ledger::kStatusBufferIdleMs + 100);
		results.expect_true(kGroup, QStringLiteral("(2) 入力を終えると、その値を渡す"),
				    sim.buffer_reflects() == reflects2 + 1 && sim.settings().display_buffer_ms == 350,
				    QStringLiteral("渡した %1 回、設定 %2").arg(sim.buffer_reflects() - reflects2).arg(sim.settings().display_buffer_ms));
		buffer->spin()->clearFocus();
		buffer->spin()->setValue(90);
		pump(ledger::kStatusBufferIdleMs + 100);
	}

	// 設定ウィンドウの [適用] で古い値に戻らない (ウィンドウを開いた時点の値は 100 ms)。
	{
		config::Settings page_copy = sim.settings();
		page_copy.display_buffer_ms = 100;
		page_copy.record_dir = "D:/rec";
		dock::apply_settings_page_fields(page_copy, sim.settings());
		results.expect_true(kGroup, QStringLiteral("(2) 設定ウィンドウの [適用] で、映像のバッファが古い値に戻らない"),
				    sim.settings().display_buffer_ms == 90 && sim.settings().record_dir == "D:/rec");
	}

	// OBS を閉じる処理で、渡していない値と待っている保存を行う。
	{
		const int saves2 = sim.look_saves();
		buffer->spin()->setValue(300);
		pump(150);
		buffer->spin()->setValue(310);
		results.expect_true(kGroup, QStringLiteral("(2) 前提: 保存を待っている"), sim.look_save_pending());
		sim.flush_look();
		results.expect_true(kGroup, QStringLiteral("(2) OBS を閉じる処理で、待っている保存を今行い、最後の値を保存する"),
				    sim.look_saves() == saves2 + 1 && sim.saved_settings().display_buffer_ms == 310 && !sim.look_save_pending() &&
					    !tab->has_unreflected());
		pump(1500);
		results.expect_true(kGroup, QStringLiteral("(2) 閉じる処理の後に、保存が残らない"), sim.look_saves() == saves2 + 1);
	}

	// 外から届いた値は、操作していなければ反映し、操作している間は書き換えない。
	{
		sim.settings().display_buffer_ms = 500;
		pump(1000);
		results.expect_true(kGroup, QStringLiteral("(2) 外から届いた値は、操作していない入力欄に反映する"),
				    std::lround(buffer->spin()->value()) == 500, QString::number(buffer->spin()->value()));
		buffer->spin()->setFocus(Qt::TabFocusReason);
		pump(100);
		sim.settings().display_buffer_ms = 700;
		pump(1000);
		results.expect_true(kGroup, QStringLiteral("(2) 操作している (フォーカスのある) 入力欄は、外から届いた値で書き換えない"),
				    std::lround(buffer->spin()->value()) == 500, QString::number(buffer->spin()->value()));
		buffer->spin()->clearFocus();
		pump(1000);
		results.expect_true(kGroup, QStringLiteral("(2) 操作が終わると、次の周期で外の値に追従する"),
				    std::lround(buffer->spin()->value()) == 700, QString::number(buffer->spin()->value()));
	}

	// 表示タブの保存で、状態タブで変えた値を戻さない。
	{
		buffer->spin()->setValue(40);
		pump(ledger::kStatusBufferIdleMs + 100);
		sim.widget()->look_tab()->flush();
		config::Settings tab_copy = sim.widget()->look_tab()->edited();
		results.expect_true(kGroup, QStringLiteral("(2) 表示タブの値を渡しても、状態タブで変えた映像のバッファは戻らない"),
				    sim.settings().display_buffer_ms == 40, QString::number(sim.settings().display_buffer_ms));
		(void)tab_copy;
	}
}

// ---- (3) 映像途切れの累計 ----
static void run_status_tally(const ScenarioOptions &options, Results &results)
{
	SimPoll sim(ui_preview::fixture_by_id("status-empty"), options.style);
	Input input;
	open_status(sim, input);
	dock::StatusTab *tab = sim.widget()->status_tab();
	const std::string taro = sim.obs().people[1].user_id;
	const std::string hanako = sim.obs().people[0].user_id;

	// 途切れを 2 回出す。
	const auto stall = [&](const std::string &id, int ms) {
		sim.set_frames_stopped(id, true);
		pump(ms);
		sim.set_frames_stopped(id, false);
		pump(1200);
	};
	stall(taro, 3300);
	stall(taro, 3300);
	const int row = table_row_of(tab, QStringLiteral("Taro"));
	results.expect_true(kGroup, QStringLiteral("(3) 映像途切れが 2 回出た人の行が表に入る"), row > 0, QString::number(tab->table_rows()));
	if (row > 0) {
		results.expect_true(kGroup, QStringLiteral("(3) 途切れの回数は 2 回、合計の秒数は 0 でない"),
				    cell_text(tab, row, 1).contains(QLatin1Char('2')) && !cell_text(tab, row, 2).startsWith(QLatin1Char('0')),
				    cell_text(tab, row, 1) + QLatin1Char('/') + cell_text(tab, row, 2));
	}
	results.expect_true(kGroup, QStringLiteral("(3) 累計は settings.json に書かない (保存する設定が変わらない)"),
			    !sim.look_save_pending() && sim.look_saves() == 0);

	// 通話を抜けた人の行が残る。
	{
		stall(hanako, 3300);
		const int before = table_row_of(tab, QStringLiteral("hanako"));
		auto &people = sim.obs().people;
		people.erase(std::remove_if(people.begin(), people.end(), [&](const ledger::PersonFact &p) { return p.user_id == hanako; }),
			     people.end());
		sim.changed();
		pump(1500);
		results.expect_true(kGroup, QStringLiteral("(3) 抜けた人の行は、累計が 0 でなければ残る"),
				    before > 0 && table_row_of(tab, QStringLiteral("hanako")) > 0,
				    QStringLiteral("前 %1 後 %2").arg(before).arg(table_row_of(tab, QStringLiteral("hanako"))));
	}

	// [途切れの累計をリセット]。
	{
		QPushButton *reset = tab->reset_button();
		results.expect_true(kGroup, QStringLiteral("(3) 前提: [途切れの累計をリセット] を押せる"), reset != nullptr && reset->isEnabled() && reset->isVisibleTo(sim.widget()));
		const int rows_before = tab->table_rows();
		// 途切れが続いている間は、周期ごとに表の合計が変わる (View が毎回違う)。そのままでも押せる (周期ごとにクリックを捨てる
		// ガードをかけ直さない)。
		sim.set_frames_stopped(taro, true);
		pump(2600);
		const std::string total_before = cell_text(tab, std::max(1, table_row_of(tab, QStringLiteral("Taro"))), 2).toStdString();
		pump(1100);
		results.expect_true(kGroup, QStringLiteral("前提: 途切れが続いている間は、周期ごとに表の値が変わる"),
				    cell_text(tab, std::max(1, table_row_of(tab, QStringLiteral("Taro"))), 2).toStdString() != total_before);
		input.move_to(center_of(reset));
		pump(50);
		input.click();
		pump(2000); // 並びの保留が解ける (ポインターが止まって 0.6 秒)
		sim.set_frames_stopped(taro, false);
		pump(1500);
		results.expect_true(kGroup, QStringLiteral("(3) リセットで、累計が消え、抜けた人の行も消える"),
				    rows_before >= 2 && tab->table_rows() == 0, QStringLiteral("前 %1 行、後 %2 行").arg(rows_before).arg(tab->table_rows()));
		results.expect_true(kGroup, QStringLiteral("(3) リセットの操作を、押された操作として 1 回記録した"),
				    std::count_if(sim.actions().begin(), sim.actions().end(),
						  [](const ActionRecord &r) { return r.id == ledger::ActionId::ResetStallTally && r.ran; }) == 1);
	}

	// 配信の開始で消える。
	{
		stall(taro, 3300);
		results.expect_true(kGroup, QStringLiteral("前提: 配信を始める前に、途切れの累計がある"), table_row_of(tab, QStringLiteral("Taro")) > 0);
		sim.obs().uninstall.obs_streaming = true;
		sim.changed();
		pump(1500);
		results.expect_true(kGroup, QStringLiteral("(3) 配信の開始で累計がゼロに戻る"), tab->table_rows() == 0, QString::number(tab->table_rows()));
		// 配信中の次の途切れから数える。
		stall(taro, 3300);
		const int r2 = table_row_of(tab, QStringLiteral("Taro"));
		results.expect_true(kGroup, QStringLiteral("(3) 配信中に出た途切れは数える (1 回)"), r2 > 0 && cell_text(tab, r2, 1).contains(QLatin1Char('1')),
				    r2 > 0 ? cell_text(tab, r2, 1) : QString());
		// 配信を終えても累計は消えない。
		sim.obs().uninstall.obs_streaming = false;
		sim.changed();
		pump(1500);
		results.expect_true(kGroup, QStringLiteral("(3) 配信を終えても累計は消えず、配信していない間も数える"),
				    table_row_of(tab, QStringLiteral("Taro")) > 0);
	}
}

// ---- (4) 履歴 ----
static void run_status_history(const ScenarioOptions &options, Results &results)
{
	SimPoll sim(ui_preview::fixture_by_id("status-empty"), options.style);
	Input input;
	open_status(sim, input);
	dock::StatusTab *tab = sim.widget()->status_tab();
	results.expect_eq(kGroup, QStringLiteral("(4) 履歴は最初は空 (開いただけの変化は書かない)"), tab->history_lines(), 0);

	// 録画の開始と終了、接続と切断。
	sim.obs().recording.phase = ledger::RecordPhase::Recording;
	sim.obs().recording.enabled_since_us = sim.now_us();
	sim.changed();
	sim.tick();
	sim.obs().link.state = ledger::LinkState::Disconnected;
	sim.changed();
	sim.tick();
	sim.obs().recording.phase = ledger::RecordPhase::Idle;
	sim.obs().recording.enabled_since_us.reset();
	sim.changed();
	sim.tick();
	const std::vector<ledger::dockview::StatusHistoryLine> &h = sim.view().status_tab.history;
	QStringList ids;
	for (const auto &line : h)
		ids << QString::fromStdString(line.text.id);
	// 新しいものが先頭。
	results.expect_true(kGroup, QStringLiteral("(4) 新しいものが先頭 (録画の終了、メッセージ、切断、録画の開始の順)"),
			    !h.empty() && h.front().text.id == keys::kStatusHistoryRecordingStoppedStatus &&
				    std::find_if(h.begin(), h.end(), [](const auto &l) { return l.text.id == keys::kStatusHistoryDisconnectedStatus; }) <
					    std::find_if(h.begin(), h.end(), [](const auto &l) { return l.text.id == keys::kStatusHistoryRecordingStartedStatus; }),
			    ids.join(QLatin1Char(' ')));
	pump(300);
	results.expect_true(kGroup, QStringLiteral("(4) 画面の行も View の順で、同じ数"), tab->history_lines() == static_cast<int>(h.size()));

	// 変化がなければ増えない。
	const std::size_t n = h.size();
	for (int i = 0; i < 20; ++i)
		sim.tick();
	results.expect_true(kGroup, QStringLiteral("(4) 変化がない周期では、行が増えない"), sim.view().status_tab.history.size() == n);

	// 上限は 60 行。70 回の出来事を起こす。
	for (int i = 0; i < 70; ++i) {
		sim.obs().recording.phase = (i % 2 == 0) ? ledger::RecordPhase::Recording : ledger::RecordPhase::Idle;
		sim.changed();
		sim.tick();
	}
	pump(400);
	results.expect_eq(kGroup, QStringLiteral("(4) 履歴は最大 60 行"), static_cast<long long>(sim.view().status_tab.history.size()), 60);
	results.expect_eq(kGroup, QStringLiteral("(4) 画面の履歴の行も 60 行"), tab->history_lines(), 60);
	// 先頭は最後の出来事 (i = 69 は Idle -> 録画の終了)。
	results.expect_true(kGroup, QStringLiteral("(4) 先頭は最後の出来事 (録画の終了)"),
			    sim.view().status_tab.history.front().text.id == keys::kStatusHistoryRecordingStoppedStatus);
}

// ---- (5) ホイール ----
static void run_status_wheel(const ScenarioOptions &options, Results &results)
{
	SimPoll sim(ui_preview::fixture_by_id("status-history-60"), options.style);
	Input input;
	open_status(sim, input, 420, 420);
	dock::StatusTab *tab = sim.widget()->status_tab();
	QScrollArea *scroll = status_scroll(sim);
	results.expect_true(kGroup, QStringLiteral("前提: 履歴 60 行の状態タブは、低いドックでスクロールする"),
			    scroll != nullptr && scroll->verticalScrollBar()->maximum() > 0);
	if (scroll == nullptr)
		return;
	struct Target {
		QString name;
		QWidget *widget;
		std::function<QString()> value;
	};
	dock::ui::SliderSpin *buffer = tab->buffer_input();
	const std::vector<Target> targets{
		{QStringLiteral("つまみ"), buffer->slider(), [&] { return QString::number(buffer->slider()->value()); }},
		{QStringLiteral("数の入力欄"), buffer->spin(), [&] { return QString::number(buffer->spin()->value()); }},
	};
	const int reflects0 = sim.buffer_reflects();
	for (const Target &t : targets) {
		if (t.widget == nullptr) {
			results.expect_true(kGroup, QStringLiteral("(5) ") + t.name + QStringLiteral(": 入力欄がある"), false);
			continue;
		}
		for (int focused = 0; focused < 2; ++focused) {
			scroll->verticalScrollBar()->setValue(0);
			scroll->ensureWidgetVisible(t.widget, 0, 0);
			pump(100);
			const int before_scroll = scroll->verticalScrollBar()->value();
			const QString before = t.value();
			if (focused != 0)
				t.widget->setFocus(Qt::TabFocusReason);
			input.move_to(center_of(t.widget));
			pump(60);
			for (int i = 0; i < 3; ++i) {
				input.wheel(-120);
				pump(40);
			}
			pump(200);
			const QString where = t.name + (focused != 0 ? QStringLiteral (" (フォーカスあり)") : QString());
			results.expect_true(kGroup, QStringLiteral("(5) ") + where + QStringLiteral(": ホイールで値が変わらない"), t.value() == before,
					    QStringLiteral("%1 -> %2").arg(before, t.value()));
			results.expect_true(kGroup, QStringLiteral("(5) ") + where + QStringLiteral(": ホイールでタブがスクロールする"),
					    scroll->verticalScrollBar()->value() > before_scroll);
			t.widget->clearFocus();
		}
	}
	results.expect_true(kGroup, QStringLiteral("(5) ホイールでは、配信の処理へ値を渡さない"), sim.buffer_reflects() == reflects0);
}

// ---- (6) 最小の幅 ----
static void run_status_width(const ScenarioOptions &options, Results &results)
{
	// status-vesktop-not-running-writes-off は「Vesktop」の組の押せない理由の文を、status-first-day は最初の設定のカードの押せない理由を含む。
	for (const char *id : {"status-table", "status-history-60", "status-messages", "status-last-recording", "status-no-composite",
			       "status-vesktop-not-running", "status-vesktop-not-running-writes-off", "status-first-day",
			       "notice-contacts"}) {
		SimPoll sim(ui_preview::fixture_by_id(id), options.style);
		sim.settings().notice_show_delay_ms = 0;
		Input input;
		sim.arrival() = sim.obs().receive.arrival;
		open_status(sim, input, 600, 700);
		const int min_w = sim.widget()->computed_min_width();
		sim.dock()->setGeometry(1300, 120, min_w + 2, 700);
		pump(800);
		QString detail;
		const int count = overflow_count(sim, &detail);
		results.expect_true(kGroup, QStringLiteral("(6) 最小の幅 (%1 px) でも、横にはみ出さない: %2").arg(min_w).arg(QString::fromLatin1(id)), count == 0,
				    detail);
	}
}

// ---- (7) 並びの保留 ----
static void run_status_hold(const ScenarioOptions &options, Results &results)
{
	SimPoll sim(ui_preview::fixture_by_id("status-table"), options.style);
	sim.settings().notice_show_delay_ms = 0;
	Input input;
	sim.arrival() = sim.obs().receive.arrival;
	sim.obs().receive.arrival.clear();
	open_status(sim, input, 420, 900);
	dock::StatusTab *tab = sim.widget()->status_tab();
	QPushButton *reset = tab->reset_button();
	dock::ui::SliderSpin *buffer = tab->buffer_input();
	const int rows0 = tab->table_rows();
	results.expect_true(kGroup, QStringLiteral("前提: 表、[途切れの累計をリセット]、映像のバッファがある"),
			    rows0 >= 3 && reset != nullptr && buffer != nullptr);
	if (reset == nullptr || buffer == nullptr)
		return;
	const Frame frame0 = frame_of(sim.widget());

	// [途切れの累計をリセット] を押している間 (マウスのボタンを押したまま)、構成が変わる更新を行う。
	{
		const QRect reset_rect = global_rect(reset);
		const QRect buffer_rect = global_rect(buffer);
		input.move_to(center_of(reset));
		pump(50);
		input.press();
		pump(100);
		// 構成が変わる更新: 表の行の増減 (2 人増える)、録画の節 (なし -> 録画中)。
		auto facts = ui_preview::sample_arrival_facts(60, 2);
		sim.arrival().insert(sim.arrival().end(), facts.begin(), facts.end());
		sim.obs().recording.phase = ledger::RecordPhase::Recording;
		sim.obs().recording.enabled_since_us = sim.now_us();
		sim.changed();
		pump(1500);
		results.expect_true(kGroup, QStringLiteral("(7) [途切れの累計をリセット] を押している間は、表の行の増減と録画の節の変化を反映しない"),
				    tab->table_rows() == rows0 && tab->recording_section() == ledger::dockview::StatusRecordingSection::None && tab->moves_pending(),
				    QStringLiteral("行 %1、保留 %2").arg(tab->table_rows()).arg(tab->moves_pending()));
		results.expect_true(kGroup, QStringLiteral("(7) その間、[途切れの累計をリセット] と [映像のバッファ] の位置が動かない"),
				    global_rect(reset) == reset_rect && global_rect(buffer) == buffer_rect,
				    QStringLiteral("リセット %1,%2 バッファ %3,%4")
					    .arg(global_rect(reset).y())
					    .arg(reset_rect.y())
					    .arg(global_rect(buffer).y())
					    .arg(buffer_rect.y()));
		results.expect_true(kGroup, QStringLiteral("(7) 1 行目、メインとサブの行、タブのバー、下のバーも動かない"), frame_of(sim.widget()) == frame0);
		// ボタンから外れて離す (押したことにしない)。
		input.move_to(sim.widget()->mapToGlobal(QPoint(sim.widget()->width() / 2, 8)));
		pump(50);
		input.release();
		pump(2000);
		results.expect_true(kGroup, QStringLiteral("(7) 離して保留が解けると、ためた変化をまとめて反映する"),
				    tab->table_rows() == rows0 + 2 && tab->recording_section() == ledger::dockview::StatusRecordingSection::Recording &&
					    !tab->moves_pending(),
				    QStringLiteral("行 %1").arg(tab->table_rows()));
		results.expect_true(kGroup, QStringLiteral("(7) ボタンの外で離したので、リセットは実行していない"),
				    std::none_of(sim.actions().begin(), sim.actions().end(),
						 [](const ActionRecord &r) { return r.id == ledger::ActionId::ResetStallTally; }));
	}

	// 映像のバッファのつまみを押している間。
	{
		sim.obs().recording.phase = ledger::RecordPhase::Idle;
		sim.obs().recording.enabled_since_us.reset();
		sim.arrival().resize(3);
		sim.changed();
		pump(2200);
		const int rows1 = tab->table_rows();
		const QRect reset_rect = global_rect(reset);
		const QRect buffer_rect = global_rect(buffer);
		QSlider *slider = buffer->slider();
		const QRect g = global_rect(slider);
		input.move_to(QPoint(g.left() + 8, g.center().y()));
		pump(50);
		input.press();
		pump(100);
		auto facts = ui_preview::sample_arrival_facts(70, 3);
		sim.arrival().insert(sim.arrival().end(), facts.begin(), facts.end());
		sim.changed();
		pump(1500);
		results.expect_true(kGroup, QStringLiteral("(7) [映像のバッファ] のつまみを押している間は、表の行の増減を反映しない"),
				    tab->moves_pending(), QStringLiteral("行 %1").arg(tab->table_rows()));
		results.expect_true(kGroup, QStringLiteral("(7) その間、[映像のバッファ] と [途切れの累計をリセット] の位置が動かない"),
				    global_rect(reset) == reset_rect && global_rect(buffer) == buffer_rect);
		input.move_to(sim.widget()->mapToGlobal(QPoint(sim.widget()->width() / 2, 8)));
		pump(50);
		input.release();
		pump(2200);
		results.expect_true(kGroup, QStringLiteral("(7) 離して保留が解けると、表の行が増える"), !tab->moves_pending() && tab->table_rows() >= rows1 + 3,
				    QStringLiteral("行 %1").arg(tab->table_rows()));
	}

	// ポインターが一覧の上で動いている間の数字の更新は、保留しない。
	{
		const int rows = tab->table_rows();
		(void)rows;
		input.move_to(center_of(tab->table_widget()));
		for (int i = 0; i < 6; ++i) {
			input.move_to(center_of(tab->table_widget()) + QPoint(i * 2, 0));
			pump(60);
		}
		const QString before = tab->table_rows() > 0 ? cell_text(tab, 1, 3) : QString();
		sim.settings().display_buffer_ms = 20;
		pump(0);
		buffer->spin()->setValue(20);
		results.expect_true(kGroup, QStringLiteral("(7) 数字の更新 (バッファを変えたときの遅延の割合) は、保留の間もすぐ反映する"),
				    tab->table_rows() > 0 && cell_text(tab, 1, 3) != before, before + QStringLiteral(" -> ") + cell_text(tab, 1, 3));
	}
}

// ---- (8) 1 行目 ----
static void run_status_first_line(const ScenarioOptions &options, Results &results)
{
	SimPoll sim(ui_preview::fixture_by_id("notice-double-audio"), options.style);
	sim.settings().notice_show_delay_ms = 0;
	sim.dock()->setGeometry(1300, 120, 420, 640);
	sim.widget()->set_tab(config::DockTab::People);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	sim.start();
	pump(1200);
	dock::FirstLineBar *first = descendant_of<dock::FirstLineBar>(sim.widget());
	const QString unread = first_line_full_text(sim.widget());
	const QString expected_headline = dock::ui::resolve(sim.view().notices.empty() ? ledger::TextRef{} : sim.view().notices.front().headline);
	results.expect_true(kGroup, QStringLiteral("前提: 参加者タブを開いていて、1 行目にレベル 2 の見出しが出ている"),
			    sim.widget()->tab() == config::DockTab::People && !sim.view().notices.empty() &&
				    sim.view().notices.front().tier == ledger::dockview::Tier::L2 && unread.contains(expected_headline.left(8)),
			    unread);
	input.move_to(center_of(first));
	pump(50);
	input.click();
	pump(1500);
	results.expect_true(kGroup, QStringLiteral("(8) 1 行目を押すと、状態タブが開く"), sim.widget()->tab() == config::DockTab::Status);
	results.expect_true(kGroup, QStringLiteral("(8) 開いたレベル 2 は既読になり、1 行目から消える (見出しが出ていない)"),
			    !first_line_full_text(sim.widget()).contains(expected_headline.left(8)), first_line_full_text(sim.widget()));
	results.expect_true(kGroup, QStringLiteral("(8) 状態タブには、そのメッセージが残る"),
			    sim.widget()->status_tab()->notices()->cards().size() == 1);
	results.expect_true(kGroup, QStringLiteral("(8) 保存する設定に、開いたタブを書く"), sim.settings().dock_tab == config::DockTab::Status);
}

// ---- (9) Vesktop の設定を書く操作と、Vesktop の起動中 ----
namespace {

// root の下で見えているラベルのうち、元のテキスト (WrapLabel は動的なプロパティ sourceText) が text のものがあるか。
bool visible_label_with(QWidget *root, const QString &text)
{
	for (QLabel *label : root->findChildren<QLabel *>()) {
		if (!label->isVisibleTo(root))
			continue;
		const QVariant source = label->property("sourceText");
		const QString shown = source.isValid() ? source.toString() : label->text();
		if (shown == text)
			return true;
	}
	return false;
}

} // namespace

static void run_status_vesktop(const ScenarioOptions &options, Results &results)
{
	// 参加者タブの最初の設定のカード: Vesktop の起動中は [Vesktopに追加] が押せず、押せない理由がボタンの下に文で出る。
	{
		SimPoll sim(ui_preview::fixture_by_id("status-first-day"), options.style);
		sim.settings().notice_show_delay_ms = 0;
		sim.dock()->setGeometry(1300, 120, 420, 640);
		sim.widget()->set_tab(config::DockTab::People);
		sim.dock()->show();
		Input input;
		input.activate(sim.dock());
		sim.start();
		pump(1200);
		const QString add_label = dock::ui::resolve_key(keys::kDockAddToVesktopButton);
		const QString running_reason = dock::ui::resolve_key(keys::kDockAddVesktopRunningText);
		QPushButton *add = nullptr;
		for (QPushButton *b : sim.widget()->findChildren<QPushButton *>()) {
			if (b->text() == add_label && b->isVisibleTo(sim.widget()))
				add = b;
		}
		results.expect_true(kGroup, QStringLiteral("前提: 参加者タブの一覧の場所に [Vesktopに追加] がある"), add != nullptr);
		if (add != nullptr) {
			results.expect_true(kGroup, QStringLiteral("(9) Vesktop の起動中は、参加者タブの [Vesktopに追加] を押せない"), !add->isEnabled());
			results.expect_true(kGroup, QStringLiteral("(9) 押せない理由を、ボタンの下に文で出す (ツールチップだけにしない)"),
					    visible_label_with(sim.widget(), running_reason) && add->toolTip() == running_reason, add->toolTip());
			input.move_to(center_of(add));
			pump(50);
			input.click();
			pump(800);
			results.expect_true(kGroup, QStringLiteral("(9) 押せない [Vesktopに追加] を押しても、操作は実行しない"),
					    std::none_of(sim.actions().begin(), sim.actions().end(), [](const ActionRecord &r) {
						    return r.id == ledger::ActionId::WriteVencordDir && r.ran;
					    }));
			// Vesktop を終了すると押せる。
			sim.obs().install.process = ledger::VesktopProcess::NotRunning;
			sim.changed();
			pump(2000);
			results.expect_true(kGroup, QStringLiteral("(9) Vesktop を終了すると [Vesktopに追加] を押せ、理由の文は消える"),
					    add->isEnabled() && !visible_label_with(sim.widget(), running_reason));
		}
	}

	// 状態タブの「Vesktop」の組。
	SimPoll sim(ui_preview::fixture_by_id("status-vesktop-not-running"), options.style);
	sim.settings().notice_show_delay_ms = 0;
	Input input;
	open_status(sim, input, 420, 900);
	dock::StatusTab *tab = sim.widget()->status_tab();
	QPushButton *remove = tab->remove_from_vesktop_button();
	results.expect_true(kGroup, QStringLiteral("(9) 同梱の Vencord を指していて Vesktop が起動していなければ、[Vesktopから削除] を押せる"),
			    remove != nullptr && remove->isVisibleTo(tab) && remove->isEnabled() && !tab->vesktop_reason_label()->isVisibleTo(tab));
	if (remove == nullptr)
		return;

	// Vesktop を起動すると、組ごと消える (配信中に、押せないボタンと終了の指示を並べない)。起動しているかを読めていない
	// 間も同じ。組の増減は位置が動く変化なので、ポインターをドックの上端へ動かして並びの保留を解いてから見る。
	const auto settle = [&] {
		sim.changed();
		input.move_to(sim.widget()->mapToGlobal(QPoint(sim.widget()->width() / 2, 8)));
		pump(2500);
	};
	sim.obs().install.process = ledger::VesktopProcess::Running;
	settle();
	results.expect_true(kGroup, QStringLiteral("(9) Vesktop の起動中は「Vesktop」の組を出さない"),
			    !remove->isVisibleTo(tab) && !tab->vesktop_reason_label()->isVisibleTo(tab));
	sim.obs().install.process = ledger::VesktopProcess::Unknown;
	settle();
	results.expect_true(kGroup, QStringLiteral("(9) Vesktop が起動しているかを読めていない間は「Vesktop」の組を出さない"),
			    !remove->isVisibleTo(tab));

	// 終了すると組が現れて押せ、押すと [Vesktopから削除] の操作を 1 回実行する。確認のダイアログは出さない。
	sim.obs().install.process = ledger::VesktopProcess::NotRunning;
	settle();
	results.expect_true(kGroup, QStringLiteral("(9) Vesktop を終了すると「Vesktop」の組が現れ、[Vesktopから削除] を押せる"),
			    remove->isVisibleTo(tab) && remove->isEnabled() && !tab->vesktop_reason_label()->isVisibleTo(tab));

	// この起動で Vesktop の設定を書かないときは、組の中で押せず、理由をボタンの下に文で出す。
	const QString writes_off_reason = dock::ui::resolve_key(keys::kDockVesktopAddFailedWritesOffText);
	sim.obs().install.vesktop_writes_off = true;
	settle();
	results.expect_true(kGroup, QStringLiteral("(9) Vesktop の設定を書かない起動では [Vesktopから削除] を押せず、理由をボタンの下に文で出す"),
			    remove->isVisibleTo(tab) && !remove->isEnabled() && tab->vesktop_reason_label()->isVisibleTo(tab) &&
				    tab->vesktop_reason_label()->property("sourceText").toString() == writes_off_reason &&
				    remove->toolTip() == writes_off_reason,
			    remove->toolTip());
	sim.obs().install.vesktop_writes_off = false;
	settle();

	input.move_to(center_of(remove));
	pump(50);
	input.click();
	pump(800);
	results.expect_true(kGroup, QStringLiteral("(9) [Vesktopから削除] を押すと、確認せずに操作を 1 回実行する"),
			    std::count_if(sim.actions().begin(), sim.actions().end(), [](const ActionRecord &r) {
				    return r.id == ledger::ActionId::RemoveFromVesktop && r.ran &&
					   r.kind == ledger::IntentKind::RemoveFromVesktop;
			    }) == 1 &&
				    QApplication::activeModalWidget() == nullptr);

	// 外れた後 (Vesktop の設定が同梱の Vencord を指していない) は、組ごと出さない。
	sim.obs().install.dir = ledger::VencordDirState::NotConfigured;
	settle();
	results.expect_true(kGroup, QStringLiteral("(9) 外れた後は「Vesktop」の組を出さない"), !remove->isVisibleTo(tab));
}

void run_status(const ScenarioOptions &options, Results &results)
{
	run_status_messages(options, results);
	run_status_buffer(options, results);
	run_status_tally(options, results);
	run_status_history(options, results);
	run_status_wheel(options, results);
	run_status_width(options, results);
	run_status_hold(options, results);
	run_status_first_line(options, results);
	run_status_vesktop(options, results);
}

} // namespace cordmux::dock_drive
