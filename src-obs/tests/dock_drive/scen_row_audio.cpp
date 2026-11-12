// SPDX-License-Identifier: Apache-2.0

// rowaudio のシナリオ。参加者タブの人の行のミュートのボタン (声と画面共有の音声) を、ポインターとキーボードで押して、次の周期の値が
// 行に出るところまで確認する。音量のつまみは音声タブにあり、audio のシナリオが確認する。
// テストデータ people-audio (合成のソースがある。hanako は画面共有の音あり、Taro はメインアカウントで声をミュート、
// さくらは画面共有の音がミュート中、kenji はミュートなし、ren は声をミュート、Vesktop のアカウントの行) と、
// people-audio-none (合成のソースがない) を使う。
//   (1) 置く行と置かない行、画面共有の音のボタンが場所を空けること (声のボタンの位置がどの行でも同じ)。
//   (2) ツールチップ、アクセシブル名、押した状態。
//   (3) ポインターとキーボード (Space、Enter) で切り替え、次の周期の値で状態が変わること。Tab の順。

#include "scenarios.h"

#include <QAccessible>
#include <QPushButton>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

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
#include "script.h"
#include "sim_poll.h"

namespace cordmux::dock_drive {

namespace {

namespace keys = ledger::text::keys;

void click_center(Input &input, QWidget *w)
{
	if (w == nullptr)
		return;
	input.move_to(w->mapToGlobal(w->rect().center()));
	input.click();
}

// 行の人の見えている名前。
QString name_of(const ledger::dockview::PersonRow &row)
{
	return dock::ui::resolve_label(row.name);
}

const ledger::dockview::PersonRow *row_named(const SimPoll &sim, const QString &name)
{
	for (const ledger::dockview::PersonRow &row : sim.view().people) {
		if (name_of(row) == name)
			return &row;
	}
	return nullptr;
}

// フォーカスの順で w の次に止まるウィジェット (見えていて、フォーカスを受けて、押せるもの)。
QWidget *next_stop(QWidget *w)
{
	QWidget *n = w->nextInFocusChain();
	for (int i = 0; i < 200 && n != nullptr && n != w; ++i, n = n->nextInFocusChain()) {
		if (n->focusPolicy() != Qt::NoFocus && n->isVisibleTo(n->window()) && n->isEnabled())
			return n;
	}
	return nullptr;
}

} // namespace

void run_row_audio(const ScenarioOptions &options, Results &results)
{
	const QString group = QStringLiteral("row-audio");

	// ---- 合成のソースがない: コンポーネントを置かない ----
	{
		SimPoll sim(ui_preview::fixture_by_id("people-audio-none"), options.style);
		sim.dock()->setGeometry(1300, 120, 420, 700);
		sim.dock()->show();
		sim.start();
		pump(1500);
		int shown = 0;
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			dock::PersonRowWidget *w = sim.people()->row_for(row.user_id);
			if (w == nullptr)
				continue;
			if (w->voice_mute()->isVisibleTo(w) || w->screen_mute()->isVisibleTo(w))
				++shown;
		}
		results.expect_true(group, QStringLiteral("(1) 合成のソースがない: どの行にもミュートのボタンを置かない"), shown == 0,
				    QStringLiteral("置いた行 %1").arg(shown));
		sim.stop();
		sim.dock()->hide();
	}

	SimPoll sim(ui_preview::fixture_by_id("people-audio"), options.style);
	sim.dock()->setGeometry(1300, 120, 420, 700);
	sim.dock()->show();
	Input input;
	input.activate(sim.dock());
	std::vector<ActionRecord> seen;
	sim.on_action = [&](const ActionRecord &rec) {
		seen.push_back(rec);
		return false; // 反映は SimPoll がする (製品の Pipeline の表の代わり)
	};
	sim.start();
	pump(1500);

	auto row_widget = [&](const QString &name) -> dock::PersonRowWidget * {
		const ledger::dockview::PersonRow *r = row_named(sim, name);
		return r != nullptr ? sim.people()->row_for(r->user_id) : nullptr;
	};
	auto user_of_name = [&](const QString &name) {
		const ledger::dockview::PersonRow *r = row_named(sim, name);
		return r != nullptr ? r->user_id : std::string();
	};
	auto last_of = [&](ledger::ActionId id) -> const ActionRecord * {
		for (auto it = seen.rbegin(); it != seen.rend(); ++it) {
			if (it->id == id)
				return &*it;
		}
		return nullptr;
	};

	const QString hanako = QStringLiteral("hanako");
	const QString taro = QStringLiteral("Taro Yamada");
	const QString sakura = QStringLiteral("さくら🌸");
	const QString kenji = QStringLiteral("kenji");

	// ---- (1) 置く行と置かない行、場所 ----
	{
		bool all = true;
		int right_ref = -1;
		bool aligned = true;
		int gap_ref = -1;
		bool gap_same = true;
		for (const ledger::dockview::PersonRow &row : sim.view().people) {
			dock::PersonRowWidget *w = sim.people()->row_for(row.user_id);
			if (w == nullptr)
				continue;
			const bool want = row.audio.applies && row.checkable;
			const bool voice_shown = w->voice_mute()->isVisibleTo(w);
			const bool screen_shown = w->screen_mute()->isVisibleTo(w);
			if (voice_shown != want || screen_shown != (want && row.audio.has_screen_audio))
				all = false;
			if (!want)
				continue;
			const int right = w->width() - w->voice_mute()->mapTo(w, QPoint(0, 0)).x();
			if (right_ref < 0)
				right_ref = right;
			else if (right != right_ref)
				aligned = false;
			// 画面共有の音のボタンの場所は、持たない行も同じ (声のボタンのすぐ右)。共有のアイコンの列も同じ。
			const int gap = w->screen_mute()->geometry().left() - w->voice_mute()->geometry().right();
			if (gap_ref < 0)
				gap_ref = gap;
			else if (gap != gap_ref)
				gap_same = false;
		}
		results.expect_true(group, QStringLiteral("(1) 声のボタンは合成のソースがある行にだけ、画面共有の音のボタンは持つ人にだけ表示される"),
				    all);
		results.expect_true(group, QStringLiteral("(1) 声のボタンの位置 (行の右端から) が、画面共有の音を持つ行と持たない行で同じ"),
				    aligned, QStringLiteral("右端から %1 px").arg(right_ref));
		results.expect_true(group, QStringLiteral("(1) 声と画面共有の音のボタンの間隔が、どの行でも同じ"), gap_same);
		// サブアカウントの行は参加者の一覧に出さない (上部のサブの行にある)。
		results.expect_true(group, QStringLiteral("(1) サブアカウントの行は一覧に出さない"),
				    row_widget(QStringLiteral("cordmux-receiver")) == nullptr);
	}

	// ---- (2) ツールチップ、アクセシブル名、押した状態 ----
	{
		dock::PersonRowWidget *h = row_widget(hanako);
		dock::PersonRowWidget *t = row_widget(taro);
		dock::PersonRowWidget *s = row_widget(sakura);
		dock::PersonRowWidget *k = row_widget(kenji);
		const QString main_note = dock::ui::resolve_key(keys::kDockRowMainVoiceMuteTip);
		const QString start_note = dock::ui::resolve_key(keys::kDockRowScreenMuteStartTip);
		results.expect_true(group, QStringLiteral("(2) メインアカウント (Taro) の声のボタンは、ミュート中で、ツールチップに既定と二重の補足がある"),
				    t != nullptr && t->voice_mute()->isChecked() && t->voice_mute()->toolTip().contains(main_note),
				    t != nullptr ? QStringLiteral("checked %1 / main %2 / tip %3")
							   .arg(t->voice_mute()->isChecked() ? 1 : 0)
							   .arg(t->view().audio.is_main_account ? 1 : 0)
							   .arg(t->voice_mute()->toolTip())
						 : QString());
		results.expect_true(group, QStringLiteral("(2) ほかの人 (hanako) の声のボタンのツールチップに補足がない"),
				    h != nullptr && !h->voice_mute()->toolTip().contains(main_note) &&
					    h->voice_mute()->toolTip() == dock::ui::resolve_key(keys::kDockRowVoiceMuteOnA11y));
		results.expect_true(group, QStringLiteral("(2) ミュート中の画面共有の音 (さくら) のツールチップに、共有のたびにミュートから始まる補足がある"),
				    s != nullptr && s->screen_mute()->isChecked() && s->screen_mute()->toolTip().contains(start_note) &&
					    s->screen_mute()->toolTip().contains(dock::ui::resolve_key(keys::kDockRowScreenMuteOffA11y)));
		results.expect_true(group, QStringLiteral("(2) ミュートしていない画面共有の音 (hanako) のツールチップは「ミュート」の操作だけ"),
				    h != nullptr && !h->screen_mute()->isChecked() &&
					    h->screen_mute()->toolTip() == dock::ui::resolve_key(keys::kDockRowScreenMuteOnA11y));
		bool names_ok = h != nullptr && k != nullptr;
		if (names_ok) {
			names_ok = h->voice_mute()->accessibleName().contains(hanako) &&
				   h->screen_mute()->accessibleName().contains(hanako) &&
				   k->voice_mute()->accessibleName().contains(kenji) &&
				   h->voice_mute()->accessibleName() != h->screen_mute()->accessibleName();
		}
		results.expect_true(group, QStringLiteral("(2) アクセシブル名は、どのミュートか (声か画面共有の音か) と人の名前を言う"), names_ok,
				    h != nullptr ? h->voice_mute()->accessibleName() + QStringLiteral(" / ") + h->screen_mute()->accessibleName()
						 : QString());
		auto state_checked = [](QWidget *w) {
			QAccessibleInterface *i = QAccessible::queryAccessibleInterface(w);
			return i != nullptr && i->state().checkable && i->state().checked;
		};
		auto state_checkable_unchecked = [](QWidget *w) {
			QAccessibleInterface *i = QAccessible::queryAccessibleInterface(w);
			return i != nullptr && i->state().checkable && !i->state().checked;
		};
		results.expect_true(group, QStringLiteral("(2) 読み上げの状態: 声のミュート中 (Taro) は「押した状態」、していない (hanako) は押していない状態"),
				    t != nullptr && h != nullptr && state_checked(t->voice_mute()) &&
					    state_checkable_unchecked(h->voice_mute()));
		results.expect_true(group, QStringLiteral("(2) 読み上げの状態: 画面共有の音のミュート中 (さくら) は「押した状態」"),
				    s != nullptr && state_checked(s->screen_mute()));
		results.expect_true(group, QStringLiteral("(2) 音量の値: 行の View にさくらの声 60、画面共有 150 が入っている"),
				    row_named(sim, sakura) != nullptr && row_named(sim, sakura)->audio.voice_volume_pct == 60 &&
					    row_named(sim, sakura)->audio.screen_volume_pct == 150);
		// Tab の順: チェック -> 声 -> 画面共有の音 -> [...]。画面共有の音を持たない行は声 -> [...]。
		auto order_ok = [&](dock::PersonRowWidget *w, bool with_screen) {
			if (w == nullptr || w->check() == nullptr)
				return false;
			QWidget *a = next_stop(w->check());
			if (a != w->voice_mute())
				return false;
			QWidget *b = next_stop(a);
			if (with_screen) {
				if (b != w->screen_mute())
					return false;
				b = next_stop(b);
			}
			return b == w->dots();
		};
		results.expect_true(group, QStringLiteral("(3) Tab の順: チェック -> 声 -> 画面共有の音 -> [...] (hanako)"), order_ok(h, true));
		results.expect_true(group, QStringLiteral("(3) Tab の順: 画面共有の音を持たない行は チェック -> 声 -> [...] (kenji)"),
				    order_ok(k, false));
	}

	Script s;
	// ---- (3) ポインターで切り替える ----
	s.then(300, [&] {
		seen.clear();
		click_center(input, row_widget(hanako)->voice_mute());
	});
	s.then(1200, [&] {
		const ActionRecord *rec = last_of(ledger::ActionId::SetVoiceMute);
		dock::PersonRowWidget *w = row_widget(hanako);
		results.expect_true(group, QStringLiteral("(3) 声のボタンを押す: SetVoiceMute を hanako に on=true で押した"),
				    rec != nullptr && rec->args.user_id == user_of_name(hanako) && rec->args.on && rec->ran);
		results.expect_true(group, QStringLiteral("(3) 次の周期の値で、声のボタンが押した状態になり、ツールチップが「ミュートを解除」になる"),
				    w != nullptr && w->voice_mute()->isChecked() &&
					    w->voice_mute()->toolTip() == dock::ui::resolve_key(keys::kDockRowVoiceMuteOffA11y));
		click_center(input, w->voice_mute());
	});
	s.then(1200, [&] {
		const ActionRecord *rec = last_of(ledger::ActionId::SetVoiceMute);
		dock::PersonRowWidget *w = row_widget(hanako);
		results.expect_true(group, QStringLiteral("(3) もう一度押す: on=false で押し、ミュートが外れる"),
				    rec != nullptr && !rec->args.on && w != nullptr && !w->voice_mute()->isChecked());
	});
	// キーボード: 画面共有の音のボタンを Space と Enter で切り替える。
	s.then(300, [&] {
		seen.clear();
		input.activate(sim.dock());
		row_widget(hanako)->screen_mute()->setFocus(Qt::TabFocusReason);
	});
	s.then(200, [&] { input.key(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" ")); });
	s.then(1200, [&] {
		const ActionRecord *rec = last_of(ledger::ActionId::SetScreenAudioMute);
		dock::PersonRowWidget *w = row_widget(hanako);
		results.expect_true(group, QStringLiteral("(3) Space: 画面共有の音のボタンが SetScreenAudioMute を on=true で押す"),
				    rec != nullptr && rec->args.on && rec->args.user_id == user_of_name(hanako));
		results.expect_true(group, QStringLiteral("(3) Space の後: 次の周期の値でボタンが押した状態になる"),
				    w != nullptr && w->screen_mute()->isChecked());
		if (w != nullptr)
			w->screen_mute()->setFocus(Qt::TabFocusReason);
		input.key(Qt::Key_Return);
	});
	s.then(1200, [&] {
		const ActionRecord *rec = last_of(ledger::ActionId::SetScreenAudioMute);
		dock::PersonRowWidget *w = row_widget(hanako);
		results.expect_true(group, QStringLiteral("(3) Enter: on=false で押し、ミュートが外れる"),
				    rec != nullptr && !rec->args.on && w != nullptr && !w->screen_mute()->isChecked());
	});
	// ミュート中の画面共有の音 (さくら) を、ポインターで解除する。
	s.then(300, [&] {
		seen.clear();
		click_center(input, row_widget(sakura)->screen_mute());
	});
	s.then(1200, [&] {
		const ActionRecord *rec = last_of(ledger::ActionId::SetScreenAudioMute);
		dock::PersonRowWidget *w = row_widget(sakura);
		results.expect_true(group, QStringLiteral("(3) ミュート中のボタン (さくらの画面共有の音) を押す: on=false で押し、解除される"),
				    rec != nullptr && !rec->args.on && w != nullptr && !w->screen_mute()->isChecked());
	});

	const bool finished = s.run(60000);
	results.expect_true(group, QStringLiteral("手順が時間内に終わった"), finished);
	sim.stop();
	sim.dock()->hide();
}

} // namespace cordmux::dock_drive
