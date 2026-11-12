// SPDX-License-Identifier: Apache-2.0

// record、a11y、menus、unload のシナリオ。record は録画のボタンの見た目、押せるか、フォーカスの戻り先を、
// a11y はスクリーンリーダーへの通知を、menus はドックのメニュー、行のメニューの「使う声」、UI Automation で
// メニューの項目を実行したときの動き、保留のメッセージの [再開]、時間のかかる操作のボタンを確認する。unload は、後片付けでフォーカスリングが使う最後の入力の監視が外れ、
// ウィンドウを開いたまま削除してもクラッシュしないことを確認する。

#include "scenarios.h"

#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <QtCore/private/qobject_p.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dock/bottom_bar.h"
#include "dock/dock_widget.h"
#include "dock/first_line.h"
#include "dock/install_dialogs.h"
#include "dock/people_list.h"
#include "dock/person_row.h"
#include "dock/settings_window.h"
#include "dock/ui/focus_ring.h"
#include "dock/ui/message_dialog.h"
#include "dock/ui/text_resolve.h"
#include "drive_env.h"
#include "input.h"
#include "ledger/text_ids.h"
#include "obs_dock.h"
#include "query.h"
#include "scen_common.h"
#include "script.h"
#include "sim_poll.h"

namespace cordmux::dock_drive {

namespace {

namespace keys = ledger::text::keys;
using ledger::RecordPhase;

void click_widget(Input &input, QWidget *w)
{
	if (w == nullptr)
		return;
	input.move_to(w->mapToGlobal(w->rect().center()));
	input.click();
}

// ---- record ----

struct RecordWatch {
	QString group;
	SimPoll *sim = nullptr;
	QElapsedTimer clock;
	std::int64_t phase_started_ms = 0;
	RecordPhase phase = RecordPhase::Idle;
	int v_pressed_early = 0;
	int v_pressed_missing = 0;
	int v_text_early = 0;
	int v_text_late = 0;
	int v_enabled = 0;
	std::set<int> widths;
	QStringList notes;

	dock::RecordButton *button() const
	{
		return dynamic_cast<dock::RecordButton *>(
			sim->widget()->findChild<QWidget *>(QStringLiteral("cordmuxRecordButton")));
	}

	void set_phase(RecordPhase p)
	{
		ledger::RecordingFacts &rec = sim->obs().recording;
		rec.phase = p;
		rec.phase_since_us = sim->now_us();
		phase = p;
		phase_started_ms = clock.elapsed();
		sim->changed();
		sim->tick();
	}

	// 20 ms ごとに呼ぶ。
	void sample()
	{
		dock::RecordButton *b = button();
		if (b == nullptr)
			return;
		const std::int64_t age = clock.elapsed() - phase_started_ms;
		const QString starting = dock::ui::resolve_key(keys::kDockRecordStartingButton);
		const QString stopping = dock::ui::resolve_key(keys::kDockRecordStoppingButton);
		const bool pressed = b->recording_look() || b->pressed_look_while_disabled();
		// 周期 1 回 (500 ms) と、0 ms の周期の遅延の余裕。
		constexpr std::int64_t kLag = 600;
		const bool recordingish = phase == RecordPhase::Recording || phase == RecordPhase::Stopping;
		if (pressed && !recordingish && age > kLag) {
			++v_pressed_early;
			notes << QStringLiteral("pressed-look in phase %1 at %2ms").arg(int(phase)).arg(age);
		}
		if (pressed && phase == RecordPhase::Starting) {
			// 保存先に書けたと返る前の、押された見た目。開始の処理中は 1 回でも不合格にする。
			++v_pressed_early;
			notes << QStringLiteral("pressed-look while starting at %1ms").arg(age);
		}
		if (!pressed && recordingish && age > kLag) {
			++v_pressed_missing;
			notes << QStringLiteral("no pressed-look in phase %1 at %2ms").arg(int(phase)).arg(age);
		}
		const QString text = b->text();
		if ((text == starting || text == stopping) && age < 1000) {
			++v_text_early;
			notes << QStringLiteral("processing text at %1ms").arg(age);
		}
		if (phase == RecordPhase::Starting && age > 1000 + kLag && text != starting) {
			++v_text_late;
			notes << QStringLiteral("starting text late at %1ms (%2)").arg(age).arg(text);
		}
		if (phase == RecordPhase::Stopping && age > 1000 + kLag && text != stopping) {
			++v_text_late;
			notes << QStringLiteral("stopping text late at %1ms (%2)").arg(age).arg(text);
		}
		// 状態ごとに押せるか。開始と終了の処理中は押せず、待機と録画中は押せる (始められなかったときと、
		// 空き容量が尽きて止まったときの見た目は、別の確認で見る)。
		if (age > kLag) {
			const bool should_enable = phase == RecordPhase::Idle || phase == RecordPhase::Recording;
			const bool should_disable = phase == RecordPhase::Starting || phase == RecordPhase::Stopping;
			if ((should_enable && !b->isEnabled()) || (should_disable && b->isEnabled())) {
				++v_enabled;
				notes << QStringLiteral("enabled=%1 in phase %2 at %3ms").arg(b->isEnabled()).arg(int(phase)).arg(age);
			}
		}
		widths.insert(b->width());
	}
};

} // namespace

void run_record(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("record");
	SimPoll sim(ui_preview::fixture_by_id("status-connected-in-call"), options.style);
	sim.obs().recording.phase = RecordPhase::Idle;
	sim.obs().recording.phase_since_us = sim.now_us();
	sim.obs().recording.save_dir_writable = true;
	sim.dock()->setGeometry(1300, 120, 420, 600);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	RecordWatch watch;
	watch.group = group;
	watch.sim = &sim;
	watch.clock.start();

	// 押されたとき、手順ごとに決めた録画の状態の移り変わり (下の plan) を流す。
	std::function<void(bool on)> plan;
	int toggles = 0;
	sim.on_action = [&](const ActionRecord &rec) {
		if (rec.kind == ledger::IntentKind::SetRecording && rec.ran) {
			++toggles;
			sim.obs().choices.recording = rec.args.on ? ledger::FeatureSwitch::On : ledger::FeatureSwitch::Off;
			if (plan)
				plan(rec.args.on);
			return true;
		}
		return false;
	};
	sim.start();
	QTimer sampler;
	sampler.setInterval(20);
	QObject::connect(&sampler, &QTimer::timeout, [&] { watch.sample(); });
	sampler.start();

	// 録画のボタンにフォーカスを置いて、キーボードで押す (処理中に押せなくなったボタンに、フォーカスが
	// 戻るかを見るため)。
	auto press_button = [&] {
		dock::RecordButton *b = watch.button();
		if (b == nullptr)
			return;
		if (QApplication::focusWidget() != b)
			b->setFocus(Qt::TabFocusReason);
		input.key(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
	};
	auto focus_on_button = [&] { return QApplication::focusWidget() == watch.button(); };
	int focus_misses = 0;
	QStringList focus_notes;
	auto check_focus = [&](const QString &where) {
		if (!focus_on_button()) {
			++focus_misses;
			QWidget *f = QApplication::focusWidget();
			focus_notes << where + QStringLiteral(":") + (f != nullptr ? f->objectName() : QStringLiteral("(none)"));
		}
	};

	Script s;
	// (a) 押す -> 保存先に書けた -> 録画中 -> 押す -> 止まった (どちらも 0.3 秒)。
	s.then(600, [&] {
		plan = [&](bool on) {
			watch.set_phase(on ? RecordPhase::Starting : RecordPhase::Stopping);
			QTimer::singleShot(300, [&, on] { watch.set_phase(on ? RecordPhase::Recording : RecordPhase::Idle); });
		};
		press_button();
	});
	s.then(1200, [&] { press_button(); });
	s.then(1200, [&] {
		check_focus(QStringLiteral("a"));
		// (d) 押してから保存先に書けるまで 1.5 秒。
		plan = [&](bool on) {
			watch.set_phase(on ? RecordPhase::Starting : RecordPhase::Stopping);
			QTimer::singleShot(on ? 1500 : 2000, [&, on] { watch.set_phase(on ? RecordPhase::Recording : RecordPhase::Idle); });
		};
		press_button();
	});
	// (f) 開始の処理中に押す (押せないので、何も起きないこと)。
	auto toggles_before = std::make_shared<int>(0);
	s.then(400, [&, toggles_before] {
		*toggles_before = toggles;
		dock::RecordButton *b = watch.button();
		if (b != nullptr) {
			input.move_to(b->mapToGlobal(b->rect().center()));
			input.click();
		}
	});
	s.then(200, [&, toggles_before] {
		results.expect_eq(group, QStringLiteral("開始の処理中に押しても何も起きない (押せない)"), toggles - *toggles_before, 0);
		input.move_to(QPoint(1800, 900));
	});
	s.then(1600, [&] {
		check_focus(QStringLiteral("d"));
		// (e) 押してから止まるまで 2 秒。
		press_button();
	});
	s.then(2800, [&] {
		check_focus(QStringLiteral("e"));
		// (h) OFF の直後に、もう一度 ON にする (止まってすぐ押す)。
		plan = [&](bool on) {
			watch.set_phase(on ? RecordPhase::Starting : RecordPhase::Stopping);
			QTimer::singleShot(200, [&, on] { watch.set_phase(on ? RecordPhase::Recording : RecordPhase::Idle); });
		};
		press_button(); // ON
	});
	s.then(1000, [&] { press_button(); }); // OFF
	s.then(700, [&] { press_button(); });  // すぐ ON
	s.then(1000, [&] {
		results.expect_true(group, QStringLiteral("OFF の直後の ON で録画中に戻る"),
				    watch.phase == RecordPhase::Recording && watch.button() != nullptr &&
					    watch.button()->recording_look());
		check_focus(QStringLiteral("h"));
		// (c) 録画中に空き容量が尽きて、録画が自分で止まった。始められなかったときと同じく、
		// 待機と同じテキストで、押された見た目にならない。
		sim.obs().recording.stopped_by_space_exhausted = true;
		watch.set_phase(RecordPhase::Idle);
	});
	s.then(1200, [&] {
		dock::RecordButton *b = watch.button();
		results.expect_true(group, QStringLiteral("自分で止まった後: 待機のテキストで、押された見た目でない"),
				    b != nullptr && !b->recording_look() &&
					    b->text() == dock::ui::resolve_key(keys::kDockRecordStartButton),
				    b != nullptr ? b->property("recordState").toString() : QString());
		// (g) 自分で止まった直後に ON にする。
		plan = [&](bool on) {
			if (on)
				sim.obs().recording.stopped_by_space_exhausted = false; // 次の録画のセッションで消える
			watch.set_phase(on ? RecordPhase::Starting : RecordPhase::Stopping);
			QTimer::singleShot(300, [&, on] { watch.set_phase(on ? RecordPhase::Recording : RecordPhase::Idle); });
		};
		press_button();
	});
	s.then(1200, [&] {
		results.expect_true(group, QStringLiteral("自分で止まった直後の ON で録画中になる"),
				    watch.phase == RecordPhase::Recording && watch.button() != nullptr && watch.button()->recording_look());
		press_button();
	});
	// (b) 押す -> 保存先に書けない (始められなかった)。
	s.then(1200, [&] {
		plan = [&](bool on) {
			if (!on)
				return;
			watch.set_phase(RecordPhase::Starting);
			QTimer::singleShot(400, [&] {
				sim.obs().recording.save_dir_writable = false;
				sim.obs().recording.unwritable_dir = "D:\\NoAccess\\cordmux";
				sim.obs().choices.recording = ledger::FeatureSwitch::Off;
				watch.set_phase(RecordPhase::StartFailed);
			});
		};
		press_button();
	});
	s.then(1500, [&] {
		dock::RecordButton *b = watch.button();
		results.expect_true(group, QStringLiteral("書けない: 押された見た目でなく、待機のテキスト"),
				    b != nullptr && !b->recording_look() && b->text() == dock::ui::resolve_key(keys::kDockRecordStartButton));
		// 保存先に書けなかったことでは押せなくしない (ledger/actions.cpp の ToggleRecording のコメント:
		// 保存先は録画を始めるたびに決め直す。書けなかったことはメッセージが表示する)。docs/ui_guidelines.md の
		// 「コントロールとキーボード操作」の録画のボタンの表の最後の行 (原因が直っていないと分かっているときは
		// 押せない) とは扱いが違うので、合否にせず記録する。
		results.info(group, QStringLiteral("書けない: 押せるか (docs/ui_guidelines.md の「コントロールとキーボード操作」の録画のボタンの表の最後の行と、ledger/actions.cpp の ToggleRecording)"),
			     b != nullptr && b->isEnabled() ? QStringLiteral("押せる") : QStringLiteral("押せない"),
			     QStringLiteral("ledger/actions.cpp のとおり (保存先は録画を始めるたびに決め直す)"));
		sim.obs().recording.save_dir_writable = true;
		sim.obs().recording.unwritable_dir.clear();
		watch.set_phase(RecordPhase::Idle);
	});
	s.then(800, [&] {});

	const bool finished = s.run(60000);
	sampler.stop();
	results.expect_true(group, QStringLiteral("手順が時間内に終わった"), finished);
	results.expect_eq(group, QStringLiteral("保存先に書けたと返る前に、押された見た目になった回数"), watch.v_pressed_early, 0);
	results.expect_eq(group, QStringLiteral("録画中と終了の処理中に、押された見た目が無かった回数"), watch.v_pressed_missing, 0);
	results.expect_eq(group, QStringLiteral("1 秒より前に処理中のテキストになった回数"), watch.v_text_early, 0);
	results.expect_eq(group, QStringLiteral("1 秒を超えても処理中のテキストにならなかった回数 (周期 1 回分の余裕を含む)"), watch.v_text_late, 0);
	results.expect_eq(group, QStringLiteral("押せるかが状態と合わなかった回数"), watch.v_enabled, 0);
	results.expect_eq(group, QStringLiteral("ボタンの幅の種類 (状態で変わらない = 1)"), static_cast<long long>(watch.widths.size()), 1,
			  [&] {
				  QStringList w;
				  for (int x : watch.widths)
					  w << QString::number(x);
				  return w.join(QLatin1Char(','));
			  }());
	results.expect_eq(group, QStringLiteral("処理が済んだ後に、フォーカスがボタンへ戻らなかった回数"), focus_misses, 0,
			  focus_notes.join(QLatin1Char(' ')));
	if (!watch.notes.isEmpty())
		results.info(group, QStringLiteral("不合格の内訳 (先頭の 20 件)"), watch.notes.mid(0, 20).join(QLatin1Char(' ')));
	sim.stop();
	sim.dock()->hide();
}

// ---- a11y ----

namespace {

struct A11yLog {
	int alerts = 0;
	QStringList popup_sources;
	struct Ann {
		QString text;
		bool assertive = false;
	};
	std::vector<Ann> announcements;
	int total = 0;
};
A11yLog *g_a11y = nullptr;

void a11y_handler(QAccessibleEvent *event)
{
	if (g_a11y == nullptr)
		return;
	++g_a11y->total;
	switch (event->type()) {
	case QAccessible::Alert:
		++g_a11y->alerts;
		break;
	case QAccessible::PopupMenuStart: {
		QObject *o = event->object();
		g_a11y->popup_sources << (o != nullptr ? QString::fromLatin1(o->metaObject()->className()) : QStringLiteral("(null)"));
		break;
	}
	case QAccessible::Announcement: {
		auto *a = static_cast<QAccessibleAnnouncementEvent *>(event);
		g_a11y->announcements.push_back(
			{a->message(), a->politeness() == QAccessible::AnnouncementPoliteness::Assertive});
		break;
	}
	default:
		break;
	}
}

} // namespace

// スクリーンリーダーへの通知を決める製品の関数 (dock::announcements_for。DockPoll と SimPoll が呼ぶ純粋関数) の
// 場合分けを、テストで作った差分で直接確認する。アクセシビリティのイベントを受け取れない環境でも実行できる。
// 期待する値は、docs/ui_guidelines.md の「メッセージのレベルと表示方法」の規則から書き、関数の返り値から作らない。
void check_announcements_for(Results &results)
{
	const QString group = QStringLiteral("a11y");
	using ledger::dockview::Tier;
	auto event = [](Tier tier, const char *key, bool is_row = false) {
		ledger::NoticeEvent e;
		e.key = key;
		e.tier = tier;
		e.headline = ledger::TextRef{key, {}};
		e.is_row = is_row;
		return e;
	};
	ledger::dockview::FirstLine summary;
	summary.forms = {ledger::TextRef{keys::kDockSummaryConnectedStatus, {}},
			 ledger::TextRef{keys::kDockSummaryConnectedShortStatus, {}}};
	const QString summary_full = dock::ui::resolve(summary.forms.front());
	const QString l1_text = dock::ui::resolve(ledger::TextRef{keys::kDockContactMediaHeadline, {}});
	const QString l1b_text = dock::ui::resolve(ledger::TextRef{keys::kDockContactRosterHeadline, {}});
	auto shape = [](const std::vector<dock::Announcement> &got) {
		QStringList out;
		for (const dock::Announcement &a : got)
			out << (a.assertive ? QStringLiteral("!") : QString()) + a.text.left(16);
		return out.isEmpty() ? QStringLiteral("(none)") : out.join(QLatin1Char(' '));
	};
	auto check = [&](const QString &label, const std::vector<dock::Announcement> &got,
			 const std::vector<dock::Announcement> &want) {
		bool ok = got.size() == want.size();
		for (std::size_t i = 0; ok && i < got.size(); ++i)
			ok = got[i].text == want[i].text && got[i].assertive == want[i].assertive;
		results.add(group, QStringLiteral("announcements_for: ") + label, shape(got), shape(want),
			    ok ? Verdict::Pass : Verdict::Fail);
	};
	{
		ledger::NoticeDiff d;
		d.appeared.push_back(event(Tier::L1, keys::kDockContactMediaHeadline));
		check(QStringLiteral("メッセージのレベル 1 が表示された: 見出しを割り込みで 1 回"), dock::announcements_for(d, summary, true),
		      {dock::Announcement{l1_text, true}});
	}
	{
		ledger::NoticeDiff d;
		d.disappeared.push_back(event(Tier::L1, keys::kDockContactMediaHeadline));
		check(QStringLiteral("メッセージのレベル 1 が消えた: 消えた後の 1 行目の全文 (いちばん長い形) を控えめで 1 回"),
		      dock::announcements_for(d, summary, true), {dock::Announcement{summary_full, false}});
	}
	{
		ledger::NoticeDiff d;
		d.disappeared.push_back(event(Tier::L1, keys::kDockContactMediaHeadline));
		d.disappeared.push_back(event(Tier::L1, keys::kDockContactRosterHeadline));
		check(QStringLiteral("同じ周期にメッセージのレベル 1 が 2 つ消えても 1 回"), dock::announcements_for(d, summary, true),
		      {dock::Announcement{summary_full, false}});
	}
	{
		// メッセージのレベル 1 が入れ替わった: 新しい見出しを割り込みで通知し、それが消えた後の
		// 1 行目でもあるので、2 回目は通知しない。
		ledger::NoticeDiff d;
		d.appeared.push_back(event(Tier::L1, keys::kDockContactRosterHeadline));
		d.disappeared.push_back(event(Tier::L1, keys::kDockContactMediaHeadline));
		ledger::dockview::FirstLine now_l1;
		now_l1.kind = ledger::dockview::FirstLineKind::Notice;
		now_l1.tier = Tier::L1;
		now_l1.forms = {ledger::TextRef{keys::kDockContactRosterHeadline, {}}};
		check(QStringLiteral("表示された見出しと、消えた後の 1 行目が同じテキストなら、2 回目は通知しない"),
		      dock::announcements_for(d, now_l1, true), {dock::Announcement{l1b_text, true}});
	}
	{
		ledger::NoticeDiff d;
		d.disappeared.push_back(event(Tier::L2, keys::kDockVersionDiffersHeadline));
		d.disappeared.push_back(event(Tier::L3, keys::kDockDoubleAudioHeadline));
		check(QStringLiteral("メッセージのレベル 2 とメッセージのレベル 3 が消えた: 通知しない"), dock::announcements_for(d, summary, true), {});
	}
	{
		ledger::NoticeDiff d;
		d.appeared.push_back(event(Tier::L1, keys::kDockContactMediaHeadline, true));
		d.disappeared.push_back(event(Tier::L1, keys::kDockContactRosterHeadline, true));
		check(QStringLiteral("人の行の状態の表示と消去: 通知しない"), dock::announcements_for(d, summary, true), {});
	}
	{
		ledger::NoticeDiff d;
		d.list_full_changed_to = true;
		check(QStringLiteral("枠がすべて使用中になった (一覧が前から表示されていた): 控えめで 1 回"),
		      dock::announcements_for(d, summary, true),
		      {dock::Announcement{dock::ui::resolve_key(keys::kDockNoFreeSlotText), false}});
		check(QStringLiteral("一覧が前に表示されていなければ、枠の変化を通知しない (一覧が表示された最初の周期)"),
		      dock::announcements_for(d, summary, false), {});
		d.list_full_changed_to = false;
		check(QStringLiteral("枠が空いた (一覧が前から表示されていた): 控えめで 1 回"), dock::announcements_for(d, summary, true),
		      {dock::Announcement{dock::ui::resolve_key(keys::kDockSlotFreeA11y), false}});
	}
}

void run_a11y(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("a11y");
	A11yLog log;
	g_a11y = &log;
	// アクセシビリティのイベントを受け取る関数を登録する (登録すると、Qt はイベントを Windows に渡さないので、
	// 音が出ない)。
	QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(&a11y_handler);
	QAccessible::setActive(true);
	const bool active = QAccessible::isActive();
	results.info(group, QStringLiteral("アクセシビリティが有効か (QAccessible::isActive)"),
		     active ? QStringLiteral("yes") : QStringLiteral("no"),
		     active ? QString()
			    : QStringLiteral("プレビュー用のプラットフォームプラグインが QPlatformAccessibility を返さないので、"
					     "Qt がイベントを、受け取る関数に渡す前に捨てる"));

	SimPoll sim(ui_preview::fixture_by_id("people-8"), options.style);
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	sim.start();
	Script s;
	// スクリーンリーダーへの通知の規則 (docs/ui_guidelines.md の「メッセージのレベルと表示方法」)。
	// 表示されたときと消えたときを分けて期待する。メッセージのレベル 1 は、表示されたときに見出しを割り込みで
	// 1 回通知する。消えたときは、消えた見出しではなく、消えた後の 1 行目の全文を控えめで 1 回通知する
	// (消えた見出しの 2 回目は、余分な通知に数える)。メッセージのレベル 2 は、表示されたときだけ控えめで通知し、
	// 消えたときは通知しない。枠がすべて使用中になったときと空いたときは、控えめで通知する。
	// 消えた後の 1 行目の全文は、通知を決める関数 (announcements_for) からではなく、その周期にドックの
	// 1 行目のウィジェット (FirstLineBar) が表示している全文から読む (通知と画面が同じテキストかを、
	// 別の所から確認するため)。
	// 受け取った通知とは、見出しのテキストごとに、受け取った順で対応させる (テキストが同じなら、
	// 手順の順に表示と消去が来る)。
	// 見出しのテキストごとに、割り込みか控えめかを 1 つだけ持つ方法はとらない。メッセージのレベル
	// 1 が消えたときの控えめな通知を、違うと数えてしまうためである。手順の区切りの時刻で対応させる方法もとらない。
	// 表示や消去の待ちが次の手順の後にかかると、正しい通知を別の区切りに数えてしまうためである。消えたときの期待を
	// announcements_for の返り値で作る方法もとらない。製品の関数の答えをそのまま期待にすると、
	// 規則の実装が誤っていても合格するためである。
	struct ExpectedAnnouncement {
		QString label; // 結果の表の項目
		QString text;  // 見出し
		bool assertive = false;
	};
	std::vector<ExpectedAnnouncement> expected;
	// メッセージのレベル 1 が消えた周期に、1 行目のウィジェットが表示していた全文 (on_tick は、描画して通知を渡した
	// 後に呼ばれる)。
	QStringList l1_cleared_first_lines;
	auto *first_line_bar =
		dynamic_cast<dock::FirstLineBar *>(sim.widget()->findChild<QWidget *>(QStringLiteral("cordmuxFirstLine")));
	sim.on_tick = [&](const ledger::StabilizeResult &stable) {
		const bool l1_cleared =
			std::any_of(stable.diff.disappeared.begin(), stable.diff.disappeared.end(), [](const ledger::NoticeEvent &e) {
				return !e.is_row && e.tier == ledger::dockview::Tier::L1;
			});
		if (l1_cleared)
			l1_cleared_first_lines << (first_line_bar != nullptr ? first_line_bar->full_text() : QString());
	};
	QString l1_headline;
	auto headline_of = [&](const char *rule) {
		for (const ledger::dockview::Notice &n : sim.view().notices) {
			if (n.rule_name == rule)
				return dock::ui::resolve(n.headline);
		}
		return QString();
	};
	// メッセージのレベル 1 を表示して (空き容量が尽きて録画が止まった)、消す。
	s.then(600, [&] {
		sim.obs().recording.stopped_by_space_exhausted = true;
		sim.changed();
	});
	s.then(2800, [&] {
		l1_headline = headline_of("RecordStoppedSpaceExhausted");
		expected.push_back(
			{QStringLiteral("メッセージのレベル 1 (RecordStoppedSpaceExhausted) が表示されたとき: 見出しを割り込み (Assertive) で"), l1_headline, true});
		sim.obs().recording.stopped_by_space_exhausted = false;
		sim.changed();
	});
	// メッセージのレベル 2 (バージョンの食い違い) を表示して、消す。
	s.then(1800, [&] {
		// メッセージのレベル 1 が消えた周期の 1 行目 (ここまでに消えている。消えるまでの待ちは、メッセージを消すまでの
		// 待ち 1 秒と周期の 0.5 秒)。
		const QString after = l1_cleared_first_lines.isEmpty() ? QString() : l1_cleared_first_lines.front();
		expected.push_back({QStringLiteral("メッセージのレベル 1 が消えたとき: 消えた後の 1 行目の全文 (ドックの 1 行目のウィジェットのテキスト) を控えめ (Polite) で"),
				    after, false});
		results.expect_eq(group, QStringLiteral("メッセージのレベル 1 が消えた周期の数 (1 回の表示と消去で 1 回)"),
				  static_cast<long long>(l1_cleared_first_lines.size()), 1);
		results.expect_true(group,
				    QStringLiteral("メッセージのレベル 1 が消えた後の 1 行目のテキストは、消えた見出しと違う (「直った」と「また起きた」を聞き分けられる)"),
				    !after.isEmpty() && after != l1_headline,
				    after.left(24) + QStringLiteral(" / ") + l1_headline.left(24));
		sim.obs().versions.discord_side = "0.0.9";
		sim.changed();
	});
	s.then(2800, [&] {
		// メッセージのレベル 2 は、表示されたときだけ控えめで通知する。消えたときは通知しない
		// (同じ見出しの 2 回目が来れば、余分な通知に数える)。
		expected.push_back({QStringLiteral("メッセージのレベル 2 (VersionRestartVesktop) が表示されたとき: 見出しを控えめ (Polite) で。消えたときは通知しない"),
				    headline_of("VersionRestartVesktop"), false});
		sim.obs().versions.discord_side = "0.1.0+dev";
		sim.changed();
	});
	// 枠がすべて使用中 -> 空く -> また使用中 (テストデータ people-8 は、4 つの枠がすべて埋まっている)。
	s.then(1800, [&] {
		int slotted = 0;
		for (const ledger::PersonFact &p : sim.obs().people)
			slotted += p.slot.has_value() ? 1 : 0;
		if (slotted == 4) {
			expected.push_back({QStringLiteral("枠が空いたとき: 控えめ (Polite) で"),
					    dock::ui::resolve_key(keys::kDockSlotFreeA11y), false});
			expected.push_back({QStringLiteral("枠がすべて使用中になったとき: 控えめ (Polite) で"),
					    dock::ui::resolve_key(keys::kDockNoFreeSlotText), false});
		}
		for (ledger::PersonFact &p : sim.obs().people) {
			if (p.slot.has_value()) {
				p.slot.reset();
				break;
			}
		}
		sim.changed();
	});
	s.then(1800, [&] {
		for (ledger::PersonFact &p : sim.obs().people) {
			if (!p.slot.has_value() && p.in_roster && !p.is_receiver) {
				std::set<int> used;
				for (const ledger::PersonFact &q : sim.obs().people) {
					if (q.slot.has_value())
						used.insert(*q.slot);
				}
				for (int k = 1; k <= 4; ++k) {
					if (used.count(k) == 0) {
						p.slot = k;
						break;
					}
				}
				break;
			}
		}
		sim.changed();
	});
	// 行のメニューとドックのメニュー (PopupMenuStart)。
	s.then_blocking(1800, [&] {
		const std::vector<std::string> users = sim.people() != nullptr ? sim.people()->shown_user_ids() : std::vector<std::string>();
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			if (row.checkable && row.check_enabled) {
				input.move_to(check_hit_rect_global(sim.people(), row.user_id).center());
				break;
			}
		}
		pump(600);
		input.press(Qt::RightButton);
		input.release(Qt::RightButton);
	});
	s.then(300, [&] { input.key(Qt::Key_Escape); });
	s.then_blocking(300, [&] { click_widget(input, sim.widget()->findChild<QPushButton *>(QStringLiteral("cordmuxDockMenuButton"))); });
	s.then(300, [&] { input.key(Qt::Key_Escape); });
	// 確認、結果、失敗のダイアログ (cordmux のダイアログ。QMessageBox は使わない)。
	s.then_blocking(300, [&] {
		QDialog *d = dock::ui::make_message_dialog(sim.widget(), dock::uninstall_confirm_spec());
		d->setAttribute(Qt::WA_DeleteOnClose, true);
		d->exec();
	});
	s.then(300, [&] { input.key(Qt::Key_Escape); });
	s.then_blocking(300, [&] {
		QDialog *d = dock::ui::make_message_dialog(sim.widget(), dock::settings_save_failed_spec());
		d->setAttribute(Qt::WA_DeleteOnClose, true);
		d->exec();
	});
	s.then(300, [&] { input.key(Qt::Key_Escape); });
	s.then(300, [&] {});
	const bool finished = s.run(60000);
	sim.stop();
	sim.on_tick = nullptr;
	sim.dock()->hide();
	QAccessible::installUpdateHandler(previous);
	g_a11y = nullptr;
	check_announcements_for(results);

	results.expect_true(group, QStringLiteral("手順が時間内に終わった"), finished);
	if (!active || log.total == 0) {
		results.info(group, QStringLiteral("通知の集計"), QStringLiteral("数えられない"),
			     QStringLiteral("アクセシビリティのイベントが、受け取る関数に 1 つも届かない (プラットフォームプラグインに QPlatformAccessibility がない)。プラグインが対応すれば数える"));
		return;
	}
	results.expect_eq(group, QStringLiteral("Alert のイベントの数 (ダイアログを開いても Alert を出さない)"), log.alerts, 0);
	int non_menu = 0;
	for (const QString &src : log.popup_sources)
		non_menu += src == QLatin1String("QMenu") ? 0 : 1;
	results.expect_eq(group, QStringLiteral("PopupMenuStart を出したもののうち、QMenu でないもの"), non_menu, 0,
			  log.popup_sources.join(QLatin1Char(',')));
	// 見出しのテキストごとに、受け取った順の、割り込みかどうかの並び。
	std::map<QString, std::vector<bool>> received;
	QStringList seen;
	for (const A11yLog::Ann &a : log.announcements) {
		seen << QStringLiteral("%1%2").arg(a.assertive ? QStringLiteral("!") : QString(), a.text.left(12));
		received[a.text].push_back(a.assertive);
	}
	auto politeness_name = [](bool assertive) {
		return assertive ? QStringLiteral("割り込み") : QStringLiteral("控えめ");
	};
	std::map<QString, std::size_t> used; // 見出しのテキストごとに、期待に対応させた数
	int mismatches = 0;
	for (const ExpectedAnnouncement &e : expected) {
		std::size_t &k = used[e.text];
		const std::vector<bool> &got = received[e.text];
		QString value = QStringLiteral("なし");
		bool ok = false;
		if (!e.text.isEmpty() && k < got.size()) {
			value = politeness_name(got[k]);
			ok = got[k] == e.assertive;
		}
		++k;
		mismatches += ok ? 0 : 1;
		results.add(group, e.label, value, politeness_name(e.assertive), ok ? Verdict::Pass : Verdict::Fail,
			    e.text.isEmpty() ? QStringLiteral("見出しが View に無かった") : e.text.left(24));
	}
	// 期待にない通知 (メッセージのレベル 2 が消えたとき、メニューとダイアログを開いたとき、同じ見出しの余分な通知、
	// 期待にない見出し)。
	int extra = 0;
	QStringList extra_notes;
	for (const auto &[text, got] : received) {
		const std::size_t k = used.count(text) != 0 ? used[text] : 0;
		for (std::size_t i = k; i < got.size(); ++i) {
			++extra;
			extra_notes << QStringLiteral("%1%2").arg(got[i] ? QStringLiteral("!") : QString(), text.left(12));
		}
	}
	results.expect_eq(group, QStringLiteral("期待にない通知の数 (メッセージのレベル 2 が消えたとき、メニュー、ダイアログ、同じ見出しの余分な通知)"), extra, 0,
			  extra_notes.join(QLatin1Char(' ')));
	// 合否は上の結果で決まる。ここは受け取った順の記録 (割り込みと控えめの違い、届かなかった通知、
	// 余分な通知の数の和)。
	results.info(group, QStringLiteral("期待と違う通知の数 (割り込みと控えめの違い、届かなかった通知、余分な通知)"), QString::number(mismatches + extra),
		     QStringLiteral("受け取った順: ") + seen.join(QLatin1Char(' ')));
}

// ---- menus ----

// View のメッセージのキーの並び (記録用)。
static QString notice_keys_of(const ledger::dockview::DockViewV2 &view)
{
	QStringList keys;
	for (const ledger::dockview::Notice &n : view.notices)
		keys << QString::fromStdString(n.key);
	return keys.join(QLatin1Char(','));
}


// 行のメニューの「使う声」のサブメニューの項目のテキスト。テストデータ people-voice-menu (ある人の声を別の人が
// 使っている) のすべての行で、行のメニューを右クリックで開き、「使う声」の項目を読んで確認する。
//   - テキストに、空の値を差し込んだ跡 (「」や "'s" の前の空) がない。声を使っている人の名前が空のままテキストを
//     作ると、「alice (既に「」の行で使っています)」のようになる。
//   - 押せない項目は、項目のテキストと違う長い理由をツールチップに持つ (長い理由はメニューのツールチップに書く)。
// 項目のテキストそのもの (括弧で短い理由を添えるか、名前だけか) は表示の規則 (ledger/) が決めるので、
// ここでは期待する値を持たない (プレビューツールの画像で確認する)。
static void run_row_voice_labels(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("row-voice-menu");
	SimPoll sim(ui_preview::fixture_by_id("people-voice-menu"), options.style);
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	sim.start();
	pump(1500);
	std::vector<std::string> users;
	for (const ledger::dockview::PersonRow &row : sim.view().people) {
		// [自動で載せる] だけのメニューの行 (サブアカウントがない構成の Vesktop のアカウント) には「使う声」がない。
		if (row.menu.auto_add_only)
			continue;
		if (!check_hit_rect_global(sim.people(), row.user_id).isEmpty())
			users.push_back(row.user_id);
	}
	auto items = std::make_shared<int>(0);
	auto disabled = std::make_shared<int>(0);
	auto opened = std::make_shared<int>(0);
	auto empty_slot = std::make_shared<QStringList>();
	auto no_tip = std::make_shared<QStringList>();
	Script s;
	for (const std::string &user : users) {
		// ポインターを動かす手順と押す手順を分ける (メニューを開く手順の中でイベントを処理すると、
		// 次の手順がその中で先に実行される。script.h)。押すのは、動かしてから 700 ms 後 (クリックを捨てる
		// ガードの時間の外)。
		s.then(300, [&, user] { input.move_to(check_hit_rect_global(sim.people(), user).center()); });
		s.then_blocking(700, [&] {
			input.press(Qt::RightButton);
			input.release(Qt::RightButton);
		});
		s.then(300, [&, items, disabled, opened, empty_slot, no_tip] {
			auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
			QMenu *voices = menu != nullptr ? menu->findChild<QMenu *>(QStringLiteral("cordmuxRowVoiceMenu")) : nullptr;
			if (voices != nullptr) {
				++*opened;
				for (QAction *a : voices->actions()) {
					if (a->isSeparator())
						continue;
					++*items;
					const QString text = a->text();
					if (text.contains(QStringLiteral("\u300c\u300d")) || text.contains(QStringLiteral(" 's ")) ||
					    text.contains(QStringLiteral("\"\"")) || text.contains(QStringLiteral("()")))
						*empty_slot << text;
					if (!a->isEnabled()) {
						++*disabled;
						const QString tip = a->toolTip();
						if (tip.isEmpty() || tip == text)
							*no_tip << text;
					}
				}
			}
			if (QWidget *popup = QApplication::activePopupWidget())
				popup->close();
		});
	}
	const bool finished = s.run(60000);
	results.expect_true(group, QStringLiteral("手順が時間内に終わった"), finished);
	results.expect_eq(group, QStringLiteral("行のメニューを開いた行の数 (表示されているすべての行)"), *opened,
			  static_cast<long long>(users.size()));
	results.info(group, QStringLiteral("「使う声」の項目の数 / 押せない項目の数"), QStringLiteral("%1 / %2").arg(*items).arg(*disabled));
	results.expect_eq(group, QStringLiteral("テキストに空の値を差し込んだ跡がある項目"), empty_slot->size(), 0,
			  empty_slot->join(QStringLiteral(" | ")));
	results.expect_eq(group, QStringLiteral("押せない項目のうち、テキストと違う理由のツールチップがないもの"), no_tip->size(), 0,
			  no_tip->join(QStringLiteral(" | ")));
	sim.stop();
	sim.dock()->hide();
}

// UI Automation (スクリーンリーダーや音声での操作) でメニューの項目を実行したとき、メニューが閉じて、その操作が
// 実行されるか。UI Automation の Invoke は、Qt のアクセシビリティの press の操作 (QAccessibleMenuItem の doAction) に
// なり、QAction::trigger だけを呼ぶ。マウスとキーボードで押したときと違い、Qt はメニューを閉じない。製品は押した
// 操作をメニューが閉じてから (QMenu::exec から戻ってから) 実行するので、製品が閉じないと、操作は実行されない。
// ドックのメニューの [設定]、行のメニューの「自動で載せる」、行のメニューの「使う声」のサブメニューの項目を
// この経路で実行する。
static void run_menu_a11y_press(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("menus-a11y-press");
	SimPoll sim(ui_preview::fixture_by_id("people-voice-menu"), options.style);
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	std::vector<ActionRecord> seen;
	sim.on_action = [&](const ActionRecord &rec) {
		seen.push_back(rec);
		// ウィンドウを開く操作は記録だけにする (ウィンドウの移り変わりは windows のシナリオで確認する)。
		return rec.kind == ledger::IntentKind::OpenSettings;
	};
	sim.start();
	pump(1500);
	// menu の項目のうち pick に合う最初の項目を、アクセシビリティの press の操作で実行する。
	auto press_by_a11y = [](QMenu *menu, const std::function<bool(QAction *)> &pick) {
		QAccessibleInterface *mi = menu != nullptr ? QAccessible::queryAccessibleInterface(menu) : nullptr;
		if (mi == nullptr)
			return false;
		for (int i = 0; i < mi->childCount(); ++i) {
			QAccessibleInterface *c = mi->child(i);
			auto *a = c != nullptr ? qobject_cast<QAction *>(c->object()) : nullptr;
			if (a == nullptr || c->actionInterface() == nullptr || !pick(a))
				continue;
			c->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
			return true;
		}
		return false;
	};
	std::string user;
	for (const ledger::dockview::PersonRow &row : sim.view().people) {
		if (row.checkable && !check_hit_rect_global(sim.people(), row.user_id).isEmpty()) {
			user = row.user_id;
			break;
		}
	}
	struct Case {
		QString name;
		bool row_menu = false;
		bool voice = false;
	};
	const std::vector<Case> cases{{QStringLiteral("ドックのメニューの [設定]"), false, false},
				      {QStringLiteral("行のメニューの「自動で載せる」"), true, false},
				      {QStringLiteral("行のメニューの「使う声」のサブメニューの項目"), true, true}};
	auto pressed = std::make_shared<std::vector<bool>>(cases.size(), false);
	auto closed = std::make_shared<std::vector<bool>>(cases.size(), false);
	auto ran = std::make_shared<std::vector<QString>>(cases.size());
	Script s;
	for (std::size_t i = 0; i < cases.size(); ++i) {
		const Case c = cases[i];
		if (c.row_menu) {
			// ポインターを動かす手順と押す手順を分け、押すのは動かしてから 700 ms 後にする (run_row_voice_labels と同じ)。
			s.then(300, [&] { input.move_to(check_hit_rect_global(sim.people(), user).center()); });
			s.then_blocking(700, [&] {
				input.press(Qt::RightButton);
				input.release(Qt::RightButton);
			});
		} else {
			s.then_blocking(300, [&] {
				click_widget(input, sim.widget()->findChild<QPushButton *>(QStringLiteral("cordmuxDockMenuButton")));
			});
		}
		s.then(300, [&, c, i, pressed] {
			seen.clear();
			auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
			bool ok = false;
			if (!c.row_menu) {
				const QString settings = dock::ui::resolve(sim.view().menu.open_settings.label);
				ok = press_by_a11y(menu, [&](QAction *a) { return a->text() == settings; });
			} else if (!c.voice) {
				ok = press_by_a11y(menu, [](QAction *a) { return a->isCheckable() && a->menu() == nullptr && a->isEnabled(); });
			} else {
				QMenu *voices = menu != nullptr ? menu->findChild<QMenu *>(QStringLiteral("cordmuxRowVoiceMenu")) : nullptr;
				ok = press_by_a11y(voices, [](QAction *a) { return !a->isSeparator() && a->isEnabled() && !a->isChecked(); });
			}
			(*pressed)[i] = ok;
		});
		s.then(400, [&, i, closed, ran] {
			(*closed)[i] = QApplication::activePopupWidget() == nullptr;
			QStringList ids;
			for (const ActionRecord &rec : seen)
				ids << QString::number(int(rec.id));
			(*ran)[i] = ids.join(QLatin1Char(','));
			// 閉じなかったときは閉じてから次へ進む (閉じると、ためた操作が実行される)。
			while (QWidget *popup = QApplication::activePopupWidget())
				popup->close();
		});
	}
	const bool finished = s.run(30000);
	results.expect_true(group, QStringLiteral("手順が時間内に終わった"), finished);
	for (std::size_t i = 0; i < cases.size(); ++i) {
		results.expect_true(group, QStringLiteral("前提: %1 をアクセシビリティの press で実行した").arg(cases[i].name), (*pressed)[i]);
		results.expect_true(group, QStringLiteral("%1: メニューが閉じる").arg(cases[i].name), (*closed)[i]);
		results.expect_true(group, QStringLiteral("%1: 操作が実行される").arg(cases[i].name), !(*ran)[i].isEmpty(),
				    (*ran)[i].isEmpty() ? QStringLiteral("操作なし") : QStringLiteral("id=%1").arg((*ran)[i]));
	}
	sim.stop();
	sim.dock()->hide();
}

// 行の [自動] のタグ。ツールチップは行のメニューの [自動で枠に載せる] と同じ文言で、マウスを受けるようにしても
// 行の動きが変わらないことを確認する。
//   - タグの上にマウスを置くと、マウスを受けるのがタグになる (ツールチップはマウスを受けたウィジェットのものが出る)。
//   - タグの上の右クリックで、行のメニューが開く (タグ以外の所と同じ)。
//   - タグの上の左クリックでは、何の操作も実行されず、チェックの状態も変わらない。
static void run_row_auto_tag(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("row-auto-tag");
	SimPoll sim(ui_preview::fixture_by_id("people-always-on"), options.style);
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	std::vector<ActionRecord> seen;
	sim.on_action = [&](const ActionRecord &rec) {
		seen.push_back(rec);
		return false;
	};
	sim.start();
	pump(1500);
	// 表示している行のうち、[自動] のタグが見えている最初の行。
	dock::PersonRowWidget *row = nullptr;
	QLabel *tag = nullptr;
	if (sim.people() != nullptr) {
		for (QLabel *label : sim.people()->findChildren<QLabel *>(QStringLiteral("cordmuxRowTag"))) {
			dock::PersonRowWidget *candidate = nullptr;
			for (QWidget *w = label->parentWidget(); w != nullptr && candidate == nullptr; w = w->parentWidget())
				candidate = dynamic_cast<dock::PersonRowWidget *>(w);
			if (tag == nullptr && candidate != nullptr &&
			    label->text() == dock::ui::resolve_key(keys::kDockRowBadgeAutoLabel) && label->isVisibleTo(candidate)) {
				row = candidate;
				tag = label;
			}
		}
	}
	results.expect_true(group, QStringLiteral("前提: [自動] のタグが表示されている"), tag != nullptr,
			    sim.people() != nullptr
				    ? QStringLiteral("タグ %1 個").arg(sim.people()->findChildren<QLabel *>(QStringLiteral("cordmuxRowTag")).size())
				    : QStringLiteral("一覧なし"));
	if (tag == nullptr) {
		sim.stop();
		sim.dock()->hide();
		return;
	}
	const QString menu_text = dock::ui::resolve_key(keys::kDockRowAutoAddMenu);
	results.expect_true(group, QStringLiteral("タグのツールチップは行のメニューの [自動で枠に載せる] と同じ文言"),
			    tag->toolTip() == menu_text && !menu_text.isEmpty(), tag->toolTip());
	QCheckBox *check = row->findChild<QCheckBox *>();
	const bool checked_before = check != nullptr && check->isChecked();
	const QPoint center = tag->mapToGlobal(tag->rect().center());
	auto hovered = std::make_shared<bool>(false);
	Script s;
	s.then(300, [&] { input.move_to(center); });
	s.then(300, [&, hovered] { *hovered = Input::widget_at(center) == tag; });
	s.then_blocking(700, [&] {
		input.press(Qt::LeftButton);
		input.release(Qt::LeftButton);
	});
	s.then(300, [&] {});
	auto menu_opened = std::make_shared<bool>(false);
	s.then_blocking(700, [&] {
		input.press(Qt::RightButton);
		input.release(Qt::RightButton);
	});
	s.then(300, [&, menu_opened] {
		*menu_opened = qobject_cast<QMenu *>(QApplication::activePopupWidget()) != nullptr;
		if (QWidget *popup = QApplication::activePopupWidget())
			popup->close();
	});
	const bool finished = s.run(60000);
	results.expect_true(group, QStringLiteral("手順が時間内に終わった"), finished);
	results.expect_true(group, QStringLiteral("タグの上では、マウスを受けるのがタグである"), *hovered);
	results.expect_true(group, QStringLiteral("タグの上の左クリックでは、何の操作も実行されない"), seen.empty(),
			    QStringLiteral("操作 %1 個").arg(seen.size()));
	results.expect_true(group, QStringLiteral("タグの上の左クリックでは、チェックの状態が変わらない"),
			    check != nullptr && check->isChecked() == checked_before);
	results.expect_true(group, QStringLiteral("タグの上の右クリックで、行のメニューが開く"), *menu_opened);
	sim.stop();
	sim.dock()->hide();
}

// 行のメニューの [全画面で表示] と、1 行目の「全画面: 名前」と [元に戻す]。
//   - 項目は [自動で枠に載せる] の次で、ツールチップを持たない。合成のソースのタイルに描かれている人 (枠に載っていて画面共有
//     している人) の行では押せ、画面共有していない人の行では押せない (項目は出す)。
//   - 押すと SetFocus が 1 回実行され、1 行目が「全画面: 名前」と [元に戻す] になり、その人の行の項目にチェックが付く。
//   - 1 行目の [元に戻す] で元の配置に戻る。
//   - 全画面の人が画面共有をやめると、自動で元の配置に戻る。
static void run_row_full_screen(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("row-full-screen");
	SimPoll sim(ui_preview::fixture_by_id("people-always-on"), options.style);
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	std::vector<ActionRecord> seen;
	sim.on_action = [&](const ActionRecord &rec) {
		seen.push_back(rec);
		return false;
	};
	sim.start();
	pump(1500);

	std::string target; // 描かれている人
	std::string idle;   // 画面共有していない人
	for (const ledger::dockview::PersonRow &row : sim.view().people) {
		const bool pressable = !row.menu.focus_toggle.disabled_reason.has_value();
		if (pressable && target.empty() && !check_hit_rect_global(sim.people(), row.user_id).isEmpty())
			target = row.user_id;
		if (!pressable && idle.empty() && !row.sharing)
			idle = row.user_id;
	}
	results.expect_true(group, QStringLiteral("前提: 描かれている人と、画面共有していない人の行がある"),
			    !target.empty() && !idle.empty());
	if (target.empty() || idle.empty()) {
		sim.stop();
		sim.dock()->hide();
		return;
	}
	const QString full_screen_name = QStringLiteral("cordmuxRowFullScreen");
	const auto item_of = [&full_screen_name](QMenu *menu) -> QAction * {
		if (menu == nullptr)
			return nullptr;
		for (QAction *a : menu->actions()) {
			if (a->objectName() == full_screen_name)
				return a;
		}
		return nullptr;
	};
	const auto focus_line = [&sim] {
		const ledger::dockview::FirstLine &line = sim.view().first_line;
		return !line.forms.empty() && line.forms[0].id == keys::kDockSummaryFullScreenStatus;
	};
	const auto restore_button = [&sim]() -> QPushButton * {
		auto *first = sim.widget()->findChild<QWidget *>(QStringLiteral("cordmuxFirstLine"));
		return first != nullptr ? first->findChild<QPushButton *>() : nullptr;
	};

	// (1) 項目の位置、押せるか、チェック、ツールチップ。
	{
		QMenu *menu = sim.people()->make_menu_for(target, sim.widget());
		QStringList names;
		if (menu != nullptr) {
			for (QAction *a : menu->actions())
				names << a->objectName();
		}
		QAction *item = item_of(menu);
		const int at = static_cast<int>(names.indexOf(full_screen_name));
		results.expect_true(group, QStringLiteral("項目は [自動で枠に載せる] の次にある"),
				    at > 0 && names.value(at - 1) == QLatin1String("cordmuxRowAutoAdd"), names.join(QLatin1Char(',')));
		results.expect_true(group, QStringLiteral("描かれている人の行の項目は押せ、チェックが付いていない"),
				    item != nullptr && item->isEnabled() && item->isCheckable() && !item->isChecked());
		results.expect_true(group, QStringLiteral("押せる項目はツールチップを持たない (Qt の既定の、項目のテキストのまま)"),
				    item != nullptr && item->toolTip() == item->text(), item != nullptr ? item->toolTip() : QString());
		results.expect_true(group, QStringLiteral("項目のテキスト"),
				    item != nullptr && item->text() == dock::ui::resolve_key(keys::kDockRowFullScreenMenu),
				    item != nullptr ? item->text() : QString());
		delete menu;
		QMenu *other = sim.people()->make_menu_for(idle, sim.widget());
		QAction *disabled = item_of(other);
		results.expect_true(group, QStringLiteral("画面共有していない人の行にも項目は出て、押せない"),
				    disabled != nullptr && !disabled->isEnabled());
		results.expect_true(group, QStringLiteral("押せない項目は、[自動で枠に載せる] と同じく理由をツールチップに表示する"),
				    disabled != nullptr &&
					    disabled->toolTip() == dock::ui::resolve_key(keys::kDockRowFullScreenUnavailableTip),
				    disabled != nullptr ? disabled->toolTip() : QString());
		delete other;
	}

	// (2) 行のメニューを右クリックで開き、項目を押す。
	Script s;
	s.then(300, [&] { input.move_to(check_hit_rect_global(sim.people(), target).center()); });
	s.then_blocking(700, [&] {
		input.press(Qt::RightButton);
		input.release(Qt::RightButton);
	});
	s.then(300, [&] {
		seen.clear();
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		QAction *item = item_of(menu);
		if (item == nullptr) {
			input.key(Qt::Key_Escape);
			return;
		}
		input.move_to(menu->mapToGlobal(menu->actionGeometry(item).center()));
		input.click();
	});
	s.then(400, [&] {
		const long long ran = std::count_if(seen.begin(), seen.end(), [&](const ActionRecord &r) {
			return r.id == ledger::ActionId::SetFocus && r.ran && r.args.user_id == target;
		});
		results.expect_eq(group, QStringLiteral("押すと SetFocus がその人で 1 回実行される"), ran, 1LL,
				  QStringLiteral("操作 %1 個").arg(seen.size()));
		results.expect_true(group, QStringLiteral("1 行目が「全画面: 名前」になる"), focus_line(),
				    first_line_full_text(sim.widget()));
		const ledger::dockview::PersonRow *row = nullptr;
		for (const ledger::dockview::PersonRow &r : sim.view().people) {
			if (r.user_id == target)
				row = &r;
		}
		const QString name = row != nullptr ? dock::ui::resolve_label(row->name) : QString();
		results.expect_true(group, QStringLiteral("1 行目の文に、その人の名前が入る"),
				    !name.isEmpty() && first_line_full_text(sim.widget()).contains(name),
				    first_line_full_text(sim.widget()));
		QPushButton *b = restore_button();
		results.expect_true(group, QStringLiteral("1 行目の右端に [元に戻す] が出る"),
				    b != nullptr && b->isVisible() && b->text() == dock::ui::resolve_key(keys::kDockFullScreenRestoreButton),
				    b != nullptr ? b->text() : QStringLiteral("(なし)"));
		QMenu *menu = sim.people()->make_menu_for(target, sim.widget());
		QAction *item = item_of(menu);
		results.expect_true(group, QStringLiteral("全画面の人の行の項目にチェックが付き、押せる (押すと戻す)"),
				    item != nullptr && item->isChecked() && item->isEnabled());
		delete menu;
	});

	// (3) 1 行目の [元に戻す] で戻る。現れた直後のクリックは捨てられるので、ポインターを外して待ってから押す。
	s.then(300, [&] {
		seen.clear();
		input.move_to(QPoint(10, 10));
		pump(600);
		click_widget(input, restore_button());
	});
	s.then(400, [&] {
		const long long ran = std::count_if(seen.begin(), seen.end(), [](const ActionRecord &r) {
			return r.id == ledger::ActionId::SetFocus && r.ran && r.args.user_id.empty();
		});
		results.expect_eq(group, QStringLiteral("[元に戻す] で SetFocus (戻す向き) が 1 回実行される"), ran, 1LL);
		results.expect_true(group, QStringLiteral("[元に戻す] の後、1 行目は全画面の行でなくなる"),
				    !focus_line() && sim.obs().choices.focus_user_id.empty(), first_line_full_text(sim.widget()));
	});

	// (4) 全画面の人が画面共有をやめると、自動で戻る。
	s.then(300, [&] {
		ledger::ActionArgs args;
		args.user_id = target;
		sim.run_action(ledger::ActionId::SetFocus, args);
	});
	s.then(400, [&] {
		results.expect_true(group, QStringLiteral("前提: もう一度全画面にした"), focus_line());
		for (ledger::PersonFact &p : sim.obs().people) {
			if (p.user_id == target) {
				p.sharing = false;
				p.sharing_known = true;
			}
		}
		sim.changed();
	});
	s.then(600, [&] {
		results.expect_true(group, QStringLiteral("画面共有をやめると、全画面をやめて 1 行目が戻る"),
				    !focus_line() && sim.obs().choices.focus_user_id.empty(), first_line_full_text(sim.widget()));
		QMenu *menu = sim.people()->make_menu_for(target, sim.widget());
		QAction *item = item_of(menu);
		results.expect_true(group, QStringLiteral("画面共有をやめた人の行の項目は押せず、チェックもない"),
				    item != nullptr && !item->isEnabled() && !item->isChecked());
		delete menu;
	});
	const bool finished = s.run(30000);
	results.expect_true(group, QStringLiteral("手順が時間内に終わった"), finished);
	sim.stop();
	sim.dock()->hide();
}

void run_menus(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("menus");
	SimPoll sim(ui_preview::fixture_by_id("notice-composite-absent"), options.style);
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	std::vector<ActionRecord> seen;
	// ウィンドウを開く操作は記録だけにする (ウィンドウの移り変わりは windows のシナリオで確認する)。
	// 時間のかかる操作は、DockPoll の install_state_ と同じく、押したら処理中にして、1.5 秒後に済ませる。
	std::int64_t busy_set_ms = -1;
	sim.on_action = [&](const ActionRecord &rec) {
		seen.push_back(rec);
		switch (rec.kind) {
		case ledger::IntentKind::OpenSettings:
		case ledger::IntentKind::OpenParticipants:
		case ledger::IntentKind::ExportBundle:
			return true;
		case ledger::IntentKind::WriteVencordDir:
			busy_set_ms = sim.elapsed_ms();
			sim.set_busy({rec.id});
			// 済んでも、メッセージとボタンは残る (失敗の状態は、テストが決めるまで変えない)。ボタンは押せる状態に戻る。
			QTimer::singleShot(1500, [&] {
				sim.set_busy({});
				sim.changed();
			});
			return true;
		default:
			return false;
		}
	};
	sim.start();
	Script s;

	// ドックのメニューの項目の並びと、押したときの操作。
	struct Item {
		QString text;
		bool separator = false;
		bool checkable = false;
		bool checked = false;
	};
	auto items = std::make_shared<std::vector<Item>>();
	s.then_blocking(600, [&] { click_widget(input, sim.widget()->findChild<QPushButton *>(QStringLiteral("cordmuxDockMenuButton"))); });
	s.then(300, [&, items] {
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (menu != nullptr) {
			for (QAction *a : menu->actions())
				items->push_back({a->text(), a->isSeparator(), a->isCheckable(), a->isChecked()});
		}
		input.key(Qt::Key_Escape);
	});
	s.then(200, [&, items] {
		const ledger::dockview::DockMenuView &m = sim.view().menu;
		const QStringList expected{dock::ui::resolve(m.open_settings.label), dock::ui::resolve(m.open_participants.label),
					   dock::ui::resolve(m.export_bundle.label),
					   QStringLiteral("-"), dock::ui::resolve(m.feature_toggle.label)};
		QStringList got;
		for (const Item &it : *items)
			got << (it.separator ? QStringLiteral("-") : it.text);
		results.expect_true(group, QStringLiteral("ドックのメニューの 4 つの項目と区切りの並び"), got == expected,
				    got.join(QStringLiteral(" | ")));
	});
	// メニューの「無効にする」が ToggleFeature を呼ぶ。
	s.then_blocking(300, [&] { click_widget(input, sim.widget()->findChild<QPushButton *>(QStringLiteral("cordmuxDockMenuButton"))); });
	s.then(300, [&] {
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		QAction *last = nullptr;
		if (menu != nullptr) {
			for (QAction *a : menu->actions()) {
				if (!a->isSeparator())
					last = a;
			}
		}
		seen.clear();
		if (last != nullptr) {
			input.move_to(menu->mapToGlobal(menu->actionGeometry(last).center()));
			input.click();
		} else {
			input.key(Qt::Key_Escape);
		}
	});
	s.then(400, [&] {
		const bool called = !seen.empty() && seen.front().id == ledger::ActionId::ToggleFeature && !seen.front().args.on;
		results.expect_true(group, QStringLiteral("メニューの「無効にする」が ToggleFeature (off) を呼ぶ"), called,
				    seen.empty() ? QStringLiteral("操作なし") : QStringLiteral("id=%1").arg(int(seen.front().id)));
		results.expect_true(group, QStringLiteral("無効にした後、1 行目に [有効にする] が表示される"),
				    sim.view().first_line.button.has_value());
	});
	s.then(300, [&] {
		seen.clear();
		auto *first = sim.widget()->findChild<QWidget *>(QStringLiteral("cordmuxFirstLine"));
		QPushButton *b = first != nullptr ? first->findChild<QPushButton *>() : nullptr;
		input.move_to(QPoint(10, 10));
		pump(600);
		click_widget(input, b);
	});
	s.then(400, [&] {
		const bool called = !seen.empty() && seen.front().id == ledger::ActionId::ToggleFeature && seen.front().args.on;
		results.expect_true(group, QStringLiteral("1 行目の [有効にする] が ToggleFeature (on) を呼ぶ"), called);
	});
	// 時間のかかる操作のボタン (DockPoll が処理中にする [Vesktopに追加] = adding_to_vesktop) を押してから、
	// 処理中の見た目 (テキストが変わるか、押せなくなる) になるまで 100 ms 以内であること (docs/ui_guidelines.md の「操作への応答と待ち時間」)。
	// 押すボタンは View のボタンのテキストで探す (メッセージのカードでも、メッセージのレベル 1 の上端のボタンでも同じ)。
	auto add_busy_probe = [&](ledger::ActionId id, const QString &name) {
		auto press_ms = std::make_shared<std::int64_t>(-1);
		auto busy_seen_ms = std::make_shared<std::int64_t>(-1);
		auto busy_button = std::make_shared<QPointer<QPushButton>>();
		auto busy_text = std::make_shared<QString>();
		s.then(2600, [&, id, name, press_ms, busy_seen_ms, busy_button, busy_text] {
			QPushButton *target = nullptr;
			for (const ledger::dockview::Notice &n : sim.view().notices) {
				for (const ledger::dockview::ViewButton &vb : n.buttons) {
					if (vb.id != id)
						continue;
					const QString text = dock::ui::resolve(vb.label);
					for (QPushButton *b : sim.widget()->findChildren<QPushButton *>()) {
						if (b->isVisible() && b->text() == text)
							target = b;
					}
				}
			}
			*busy_button = target;
			if (target == nullptr) {
				// 原因を切り分けるための記録: View のメッセージとボタンの操作、表示されている
				// ボタンのテキスト。
				QStringList seen_notices;
				for (const ledger::dockview::Notice &n : sim.view().notices) {
					QStringList ids;
					for (const ledger::dockview::ViewButton &vb : n.buttons)
						ids << QString::number(static_cast<int>(vb.id));
					seen_notices << QStringLiteral("%1[%2]").arg(QString::fromStdString(n.key), ids.join(QLatin1Char(',')));
				}
				QStringList texts;
				for (QPushButton *b : sim.widget()->findChildren<QPushButton *>()) {
					if (b->isVisible())
						texts << b->text();
				}
				results.add(group, QStringLiteral("%1 のボタンが見つかる").arg(name), QStringLiteral("no"),
					    QStringLiteral("yes"), Verdict::Fail,
					    QStringLiteral("メッセージ %1 / ボタン %2").arg(seen_notices.join(QLatin1Char(' ')), texts.join(QLatin1Char('|'))));
				return;
			}
			*busy_text = target->text();
			input.move_to(target->mapToGlobal(target->rect().center()));
			pump(600);
			// 2 ms ごとに見て、テキストか押せるかが変わった時刻を記録する。
			auto *poll = new QTimer();
			poll->setInterval(2);
			QObject::connect(poll, &QTimer::timeout, [&, poll, press_ms, busy_seen_ms, busy_button, busy_text] {
				if (busy_button->isNull() || *busy_seen_ms >= 0 || sim.elapsed_ms() - *press_ms > 2000) {
					poll->deleteLater();
					return;
				}
				if (!(*busy_button)->isEnabled() || (*busy_button)->text() != *busy_text)
					*busy_seen_ms = sim.elapsed_ms();
			});
			*press_ms = sim.elapsed_ms();
			input.click();
			if (!(*busy_button)->isEnabled() || (*busy_button)->text() != *busy_text)
				*busy_seen_ms = sim.elapsed_ms();
			poll->start();
		});
		s.then(600, [&, name, press_ms, busy_seen_ms] {
			if (*press_ms < 0)
				return; // ボタンが見つからなかった (上で不合格に数えた)
			const std::int64_t d = *busy_seen_ms >= 0 ? *busy_seen_ms - *press_ms : -1;
			results.add(group, QStringLiteral("%1 を押してから処理中の見た目になるまで (ms)").arg(name), QString::number(d),
				    QStringLiteral("0-100"), d >= 0 && d <= 100 ? Verdict::Pass : Verdict::Fail,
				    QStringLiteral("テストの中の処理中のフラグ (DockPoll の install_state_ の代わり) を、押した操作で立て、次の 0 ms の周期で描画する。"
						   "製品のインストールの処理がフラグを立てるタイミング (dock/dock_poll_install.cpp) は組み込めない"));
		});
	};
	// [Vesktopに追加]: 前の [Vesktopに追加] が失敗したメッセージ (VesktopAddFailed) のボタン。このテストデータは通話中
	// (接続の状態は Connected) なので、接続の状態を問わずにボタンを表示する規則 (ledger/rules_dock.cpp の
	// added_rule_to_view) を使う。済んだら、一致している状態に戻す。
	s.then(1600, [&] {
		// ポインターを一覧の上から外す (上に置いたままだと、メッセージの表示と消去が並びの保留でためられる)。
		input.move_to(QPoint(10, 10));
		// メッセージのボタンは状態タブにある。
		sim.widget()->set_tab(config::DockTab::Status);
		sim.obs().install.dir = ledger::VencordDirState::NotConfigured;
		sim.obs().ops.vesktop_add_failure = ledger::VesktopAddFailure::StateWriteFailed;
		// [Vesktopに追加] は Vesktop の起動中は押せない (ledger/actions.cpp の vesktop_busy_reason)。このテストデータは
		// Vesktop が起動しているので、押せるように Vesktop を終了した状態にする。
		sim.obs().install.process = ledger::VesktopProcess::NotRunning;
		sim.changed();
	});
	add_busy_probe(ledger::ActionId::WriteVencordDir, QStringLiteral("[Vesktopに追加]"));
	s.then(1600, [&] {
		// 押したボタンの上からポインターを外す (上に置いたままだと、次のメッセージの表示と消去が、
		// 並びの保留でためられたままになる)。
		input.move_to(QPoint(10, 10));
		sim.obs().install.dir = ledger::VencordDirState::Matches;
		sim.obs().ops.vesktop_add_failure.reset();
		sim.changed();
	});
	// 録画のボタンと同じ規則 (処理中に押せなくしたボタンに、済んだらフォーカスを戻す) を、
	// メッセージのボタンで確認する。[Vesktopに追加] にキーボードでフォーカスを置いて Space を押し、
	// 追加できずに終わったとき (ボタンが残る) に、フォーカスがボタンへ戻るか。
	auto kb_button = std::make_shared<QPointer<QPushButton>>();
	s.then(2500, [&, kb_button] {
		sim.obs().install.dir = ledger::VencordDirState::NotConfigured;
		sim.obs().ops.vesktop_add_failure = ledger::VesktopAddFailure::StateWriteFailed;
		sim.obs().install.process = ledger::VesktopProcess::NotRunning; // 押せる状態 (上の [Vesktopに追加] と同じ)
		sim.changed();
	});
	s.then(2800, [&, kb_button] {
		for (const ledger::dockview::Notice &n : sim.view().notices) {
			for (const ledger::dockview::ViewButton &vb : n.buttons) {
				if (vb.id != ledger::ActionId::WriteVencordDir)
					continue;
				const QString text = dock::ui::resolve(vb.label);
				for (QPushButton *b : sim.widget()->findChildren<QPushButton *>()) {
					if (b->isVisible() && b->text() == text)
						*kb_button = b;
				}
			}
		}
		if (kb_button->isNull()) {
			QStringList texts;
			for (QPushButton *b : sim.widget()->findChildren<QPushButton *>()) {
				if (b->isVisible())
					texts << b->text();
			}
			results.add(group, QStringLiteral("メッセージのボタン: [Vesktopに追加] のボタンが見つかる"), QStringLiteral("no"), QStringLiteral("yes"),
				    Verdict::Fail,
				    QStringLiteral("メッセージ %1 / 並びの保留 %2 / ボタン %3")
					    .arg(notice_keys_of(sim.view()))
					    .arg(sim.widget()->layout_held() ? 1 : 0)
					    .arg(texts.join(QLatin1Char('|'))));
			return;
		}
		input.activate(sim.dock());
		(*kb_button)->setFocus(Qt::TabFocusReason);
		input.key(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
	});
	s.then(2200, [&, kb_button] {
		results.expect_true(group, QStringLiteral("メッセージのボタン: 処理が済んで押せる状態に戻ったら、フォーカスがボタンへ戻る (録画のボタンと同じ規則)"),
				    !kb_button->isNull() && (*kb_button)->isEnabled() && QApplication::focusWidget() == kb_button->data(),
				    QStringLiteral("フォーカス %1 (テキスト %2) / 押したボタン %3 / メッセージ %4")
					    .arg(QApplication::focusWidget() != nullptr ? QApplication::focusWidget()->objectName() +
											      QLatin1Char('/') +
											      QString::fromLatin1(QApplication::focusWidget()->metaObject()->className())
											: QStringLiteral("(none)"),
						 qobject_cast<QPushButton *>(QApplication::focusWidget()) != nullptr
							 ? qobject_cast<QPushButton *>(QApplication::focusWidget())->text()
							 : QString(),
						 kb_button->isNull() ? QStringLiteral("(消えた)")
								     : ((*kb_button)->isEnabled() ? QStringLiteral("押せる") : QStringLiteral("押せない")),
						 notice_keys_of(sim.view())));
	});
	const bool finished = s.run(60000);
	results.expect_true(group, QStringLiteral("手順が時間内に終わった"), finished);
	sim.stop();
	sim.dock()->hide();
	run_row_voice_labels(options, results);
	run_menu_a11y_press(options, results);
	run_row_auto_tag(options, results);
	run_row_full_screen(options, results);
	// 平常の 1 行の「表示中」の人数を確認する (scen_glance.cpp)。
	run_summary(options, results);
	run_row_audio(options, results);
}

// ---- unload ----

void run_unload(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("unload");
	// アプリケーション全体のイベントフィルターの数 (外したイベントフィルターは、Qt が null にしておいて後で
	// 詰めるので、null でないものだけを数える)。
	auto filters = [] {
		QObjectPrivate *d = QObjectPrivate::get(qApp);
		int n = 0;
		if (d->extraData != nullptr) {
			for (const QPointer<QObject> &f : d->extraData->eventFilters)
				n += f.isNull() ? 0 : 1;
		}
		return n;
	};
	const int base = filters();
	auto sim = std::make_unique<SimPoll>(ui_preview::fixture_by_id("people-8"), options.style);
	sim->dock()->setGeometry(1300, 120, 420, 700);
	sim->dock()->show();
	Input input;
	input.activate(sim->dock());
	sim->start();
	pump(400);
	// 設定ウィンドウを開いたまま (モーダルでない) にしておく。確認のダイアログ (フォーカスリングを持つ) も
	// 開いたままにする。
	dock::SettingsWindowInput in;
	in.current = sim->settings();
	in.effective_at_start = sim->settings();
	in.about = sim->view().about;
	QPointer<dock::SettingsWindow> settings = dock::make_settings_window(main_window(), in);
	settings->move(80, 60);
	settings->show();
	QPointer<QDialog> confirm = dock::ui::make_message_dialog(settings, dock::uninstall_confirm_spec());
	confirm->setModal(false);
	confirm->show();
	input.key(Qt::Key_Tab);
	pump(200);
	const int with_rings = filters();
	// obs_module_unload と同じく、フォーカスリングが残っていても、最後の入力の監視を外す。
	dock::ui::release_focus_input_watch();
	const int after_release = filters();
	results.expect_eq(group, QStringLiteral("後片付けで、フォーカスリングが使う最後の入力の監視 (アプリケーション全体のイベントフィルター) が外れる"), with_rings - after_release, 1,
			  QStringLiteral("イベントフィルターの数: 始め %1、フォーカスリングあり %2、外した後 %3 (テストのイベントフィルターを含む)").arg(base).arg(with_rings).arg(after_release));
	// 外した後にフォーカスリングを作っても、監視を登録し直さない。
	QPointer<QDialog> another = dock::ui::make_message_dialog(sim->widget(), dock::settings_save_failed_spec());
	another->setModal(false);
	another->show();
	pump(200);
	results.expect_eq(group, QStringLiteral("外した後にフォーカスリングを作っても、監視を登録し直さない"), filters(), after_release);
	// OBS が終了するときと同じく、ウィンドウを開いたまま、ドックとウィンドウを削除する。その後にキーを送っても、
	// クラッシュしないこと。
	input.key(Qt::Key_Tab);
	delete another.data();
	delete confirm.data();
	delete settings.data();
	sim->stop();
	sim.reset();
	pump(200);
	input.key(Qt::Key_Tab);
	pump(100);
	results.expect_true(group, QStringLiteral("ウィンドウを開いたまま削除しても、クラッシュしない"), true);
}

} // namespace cordmux::dock_drive
