// SPDX-License-Identifier: Apache-2.0

// audio のシナリオ。音声タブ (声と画面共有の音声のつまみ、メーター、ミュートのボタン) を、ポインターとキーボードで操作して、
// 操作が次の周期の値に出るところまで確認する。テストデータ audio-tab (話している人、ミュート中に話している人、音量 0、
// 音が大きい人、共有していない人、まだ声が届いていない人)、audio-tab-30 (30 人)、audio-tab-off、audio-tab-no-source、
// audio-tab-empty を使う。
//   (1) メーターを読む周期は、参加者タブと音声タブが見えている間だけ動き、ほかのタブでは止まる。
//   (2) つまみの上のホイールで値が変わらず、ページがスクロールする (フォーカスがあっても同じ)。
//   (3) ダブルクリックで 100% に戻る。クリックと Tab でフォーカスを取り、矢印のキーはフォーカスがあるときだけ動く (上で増え、
//       下で減る)。Home と End で両端に動く。
//   (4) つまみを動かすと SetVoiceVolume と SetScreenAudioVolume を実行する (動かす間は間引き、離したときに最後の値を送る)。
//   (5) 1 人の高さと、ボタンの位置が、共有の有無と声が届いているかで変わらない。
//   (6) ミュートのボタンの操作。声が届いていない人のボタンは押せない。
//   (7) メーターは、音を出している間は掛けた後の値を色付きで、ミュート中と音量 0 の間は掛ける前の値を灰色で出す。値が変わった
//       メーターだけを塗り直す。
//   (8) つまみを動かしている間は、行が動かず、つまみが外からの値で書き換わらない。離して止まったら反映する。
//   (9) 接続が切れた行、合成のソースがないとき、行がないときの表示。Tab の順。アクセシブル名。最小幅。

#include "scenarios.h"

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QFontMetricsF>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dock/audio_tab.h"
#include "dock/dock_widget.h"
#include "dock/ui/dock_metrics.h"
#include "dock/ui/elide_label.h"
#include "dock/ui/level_meter.h"
#include "dock/ui/text_resolve.h"
#include "dock/ui/volume_slider.h"
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

const QString kGroup = QStringLiteral("audio");

using dock::AudioKind;
using dock::AudioRowWidget;

QPoint center_of(QWidget *w)
{
	return w->mapToGlobal(w->rect().center());
}

void click_center(Input &input, QWidget *w)
{
	input.move_to(center_of(w));
	input.click();
}

QString name_of(const ledger::dockview::AudioRowView &row)
{
	return dock::ui::resolve_label(row.name);
}

const ledger::dockview::AudioRowView *view_row_named(const SimPoll &sim, const QString &name)
{
	for (const ledger::dockview::AudioRowView &row : sim.view().audio_tab.rows) {
		if (name_of(row) == name)
			return &row;
	}
	return nullptr;
}

std::string user_of_name(const SimPoll &sim, const QString &name)
{
	const ledger::dockview::AudioRowView *row = view_row_named(sim, name);
	return row != nullptr ? row->user_id : std::string();
}

AudioRowWidget *row_widget(SimPoll &sim, const QString &name)
{
	const ledger::dockview::AudioRowView *row = view_row_named(sim, name);
	return row != nullptr && sim.audio() != nullptr ? sim.audio()->row_for(row->user_id) : nullptr;
}

ledger::PersonFact *person_named(SimPoll &sim, const QString &name)
{
	const std::string id = user_of_name(sim, name);
	for (ledger::PersonFact &p : sim.obs().people) {
		if (p.user_id == id)
			return &p;
	}
	return nullptr;
}

QScrollArea *audio_scroll(SimPoll &sim)
{
	return sim.widget()->findChild<QScrollArea *>(QStringLiteral("cordmuxAudioScroll"));
}

QWidget *next_stop(QWidget *w)
{
	QWidget *n = w->nextInFocusChain();
	for (int i = 0; i < 400 && n != nullptr && n != w; ++i, n = n->nextInFocusChain()) {
		if (n->focusPolicy() != Qt::NoFocus && n->isVisibleTo(n->window()) && n->isEnabled())
			return n;
	}
	return nullptr;
}

// 1 人の行を、行の左上からの位置と大きさで表す (幅が同じ行の間で比べる)。
QRect rect_in_row(QWidget *part, QWidget *row)
{
	return QRect(part->mapTo(row, QPoint(0, 0)), part->size());
}

} // namespace

void run_audio(const ScenarioOptions &options, Results &results)
{
	const QString hanako = QStringLiteral("hanako");
	const QString taro = QStringLiteral("Taro Yamada");
	const QString sakura = QStringLiteral("さくら🌸");
	const QString kenji = QStringLiteral("kenji");
	const QString ren = QStringLiteral("ren");
	const QString mei = QStringLiteral("mei");
	const QString yuu = QStringLiteral("yuu");

	const ui_preview::Fixture fixture = ui_preview::fixture_by_id("audio-tab");
	pipeline::AudioLevels fixture_levels;
	fixture_levels.tracks = fixture.levels;
	fixture_levels.speaking = fixture.speaking;

	// ---- (1) メーターを読む周期 ----
	{
		SimPoll sim(fixture, options.style);
		sim.dock()->setGeometry(1300, 120, 420, 900);
		sim.set_levels(fixture_levels);
		sim.enable_level_timer();
		sim.widget()->set_tab(config::DockTab::Audio);
		sim.dock()->show();
		sim.start();
		pump(1500);
		results.expect_true(kGroup, QStringLiteral("(1) 音声タブが見えている間は、周期を動かす"),
				    sim.widget()->levels_wanted() && sim.level_timer_running());
		AudioRowWidget *h = row_widget(sim, hanako);
		const int reads_audio = sim.level_reads();
		results.expect_true(kGroup, QStringLiteral("(1) 周期が値を読み、メーターが動く (hanako の声は -30 dB)"),
				    reads_audio >= 2 && h != nullptr && h->meter(AudioKind::Voice)->position() > 0.3 &&
					    h->meter(AudioKind::Voice)->position() < 0.4,
				    h != nullptr ? QStringLiteral("読んだ回数 %1、位置 %2")
							   .arg(reads_audio)
							   .arg(h->meter(AudioKind::Voice)->position(), 0, 'f', 3)
					     : QString());

		sim.widget()->set_tab(config::DockTab::Display);
		pump(50);
		const int reads_stop = sim.level_reads();
		pump(450);
		results.expect_true(kGroup, QStringLiteral("(1) 表示タブを開くと、周期を止め、読まない"),
				    !sim.level_timer_running() && sim.level_reads() == reads_stop);
		results.expect_true(kGroup, QStringLiteral("(1) 周期を止めたら、メーターを 0 に戻す"),
				    h != nullptr && h->meter(AudioKind::Voice)->position() == 0.0);
		sim.widget()->set_tab(config::DockTab::Status);
		pump(50);
		results.expect_true(kGroup, QStringLiteral("(1) 状態タブでも止まっている"), !sim.level_timer_running());
		sim.widget()->set_tab(config::DockTab::Audio);
		pump(350);
		results.expect_true(kGroup, QStringLiteral("(1) 音声タブへ戻ると、周期を動かし、メーターが動く"),
				    sim.level_timer_running() && h != nullptr && h->meter(AudioKind::Voice)->position() > 0.3);

		// 参加者タブはマイクの光のために周期を動かすが、メーターは動かさない。
		sim.widget()->set_tab(config::DockTab::People);
		pump(350);
		results.expect_true(kGroup, QStringLiteral("(1) 参加者タブでは周期が動き、音声タブのメーターは 0 のまま"),
				    sim.level_timer_running() && h != nullptr && h->meter(AudioKind::Voice)->position() == 0.0);
		sim.widget()->set_tab(config::DockTab::Audio);
		sim.dock()->hide();
		pump(50);
		results.expect_true(kGroup, QStringLiteral("(1) ドックが見えなくなると、周期を止める"), !sim.level_timer_running());
		sim.dock()->show();
		pump(150);
		results.expect_true(kGroup, QStringLiteral("(1) ドックが見えると、また動かす"), sim.level_timer_running());
		sim.stop();
		sim.dock()->hide();
	}

	// ---- 音声タブを開いた 1 つのドック (4 人以上、行ごとの状態がある) ----
	SimPoll sim(fixture, options.style);
	sim.dock()->setGeometry(1300, 120, 420, 900);
	sim.settings().layout_hold_idle_ms = 600;
	sim.widget()->set_tab(config::DockTab::Audio);
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
	dock::AudioTab *audio = sim.audio();
	results.expect_true(kGroup, QStringLiteral("前提: 音声タブの中身がある"), audio != nullptr);
	if (audio == nullptr)
		return;
	auto last_of = [&](ledger::ActionId id) -> const ActionRecord * {
		for (auto it = seen.rbegin(); it != seen.rend(); ++it) {
			if (it->id == id)
				return &*it;
		}
		return nullptr;
	};
	auto count_of = [&](ledger::ActionId id) {
		return static_cast<int>(std::count_if(seen.begin(), seen.end(), [&](const ActionRecord &r) { return r.id == id; }));
	};

	// ---- (9) 並び、ミュートのボタンの位置、アクセシブル名、Tab の順 ----
	{
		std::vector<std::string> expected;
		expected.push_back(user_of_name(sim, taro));
		results.expect_true(kGroup, QStringLiteral("(9) 先頭はメインアカウント (Taro)"),
				    !audio->shown_user_ids().empty() && audio->shown_user_ids().front() == expected.front());
		std::vector<QString> names;
		for (const std::string &id : audio->shown_user_ids()) {
			AudioRowWidget *row = audio->row_for(id);
			names.push_back(row != nullptr ? row->name_label()->full_text() : QString());
		}
		const std::vector<QString> want = {taro, hanako, kenji, mei, ren, yuu, sakura, QStringLiteral("とても長い表示名の人がいるときの例です")};
		results.expect_true(kGroup, QStringLiteral("(9) 残りは表示名の順 (hanako、kenji、mei、ren、yuu、さくら、長い名前)"), names == want,
				    names.empty() ? QString() : names.at(1) + QStringLiteral(" ... ") + names.back());
		results.expect_true(kGroup, QStringLiteral("(9) サブアカウントと Vesktop のアカウントの行は出さない"),
				    audio->row_for(user_of_name(sim, QStringLiteral("cordmux-receiver"))) == nullptr &&
					    audio->shown_user_ids().size() == want.size());
		results.expect_true(kGroup, QStringLiteral("(9) 行があるので、行がないときの文は出さない"), !audio->notice_shown());

		AudioRowWidget *h = row_widget(sim, hanako);
		AudioRowWidget *k = row_widget(sim, kenji);
		AudioRowWidget *m = row_widget(sim, mei);
		AudioRowWidget *y = row_widget(sim, yuu);
		const QString name = hanako;
		bool names_ok = h != nullptr;
		if (h != nullptr) {
			names_ok = h->slider(AudioKind::Voice)->accessibleName().contains(name) &&
				   h->slider(AudioKind::Screen)->accessibleName().contains(name) &&
				   h->mute(AudioKind::Voice)->accessibleName().contains(name) &&
				   h->mute(AudioKind::Screen)->accessibleName().contains(name) &&
				   h->slider(AudioKind::Voice)->accessibleName() != h->slider(AudioKind::Screen)->accessibleName() &&
				   h->mute(AudioKind::Voice)->accessibleName() != h->mute(AudioKind::Screen)->accessibleName();
		}
		results.expect_true(kGroup, QStringLiteral("(9) つまみとミュートのボタンのアクセシブル名は、声か画面共有の音声かと人の名前を言う"),
				    names_ok,
				    h != nullptr ? h->slider(AudioKind::Voice)->accessibleName() + QStringLiteral(" / ") +
							   h->mute(AudioKind::Screen)->accessibleName()
					     : QString());
		auto state_of = [](QWidget *w, bool *checkable, bool *checked) {
			QAccessibleInterface *i = QAccessible::queryAccessibleInterface(w);
			*checkable = i != nullptr && i->state().checkable;
			*checked = i != nullptr && i->state().checked;
		};
		bool taro_checkable = false;
		bool taro_checked = false;
		bool h_checkable = false;
		bool h_checked = true;
		AudioRowWidget *t = row_widget(sim, taro);
		if (t != nullptr)
			state_of(t->mute(AudioKind::Voice), &taro_checkable, &taro_checked);
		if (h != nullptr)
			state_of(h->mute(AudioKind::Voice), &h_checkable, &h_checked);
		results.expect_true(kGroup, QStringLiteral("(9) 読み上げの状態: 声のミュート中 (Taro) は押した状態、していない (hanako) は押していない状態"),
				    taro_checkable && taro_checked && h_checkable && !h_checked);

		// Tab の順: 声のつまみ -> 声のミュート -> 画面共有のつまみ -> 画面共有のミュート -> 次の人の声のつまみ。
		bool order_ok = h != nullptr && k != nullptr;
		if (order_ok) {
			QWidget *a = next_stop(h->slider(AudioKind::Voice));
			QWidget *b = a != nullptr ? next_stop(a) : nullptr;
			QWidget *c = b != nullptr ? next_stop(b) : nullptr;
			QWidget *d = c != nullptr ? next_stop(c) : nullptr;
			order_ok = a == h->mute(AudioKind::Voice) && b == h->slider(AudioKind::Screen) &&
				   c == h->mute(AudioKind::Screen) && d == k->slider(AudioKind::Voice);
		}
		results.expect_true(kGroup, QStringLiteral("(9) Tab の順: 声のつまみ、声のミュート、画面共有のつまみ、画面共有のミュート、次の人"), order_ok);
		bool skip_ok = k != nullptr && m != nullptr;
		if (skip_ok) {
			// 共有していない人 (kenji) の画面のつまみとボタン、声が届いていない人 (mei) の声のつまみと、押せない声のボタン、
			// 画面のつまみとボタンには止まらない。kenji の声のミュートの次は ren の声のつまみである。
			skip_ok = next_stop(k->slider(AudioKind::Voice)) == k->mute(AudioKind::Voice) &&
				  next_stop(k->mute(AudioKind::Voice)) == row_widget(sim, ren)->slider(AudioKind::Voice);
		}
		results.expect_true(kGroup, QStringLiteral("(9) 文を置いた行と押せないボタンには、Tab で止まらない"), skip_ok);

		// 文の場所。
		results.expect_true(kGroup, QStringLiteral("(9) 共有していない人の画面の行は「共有していません」を、声が届いていない人の声の行は「まだ声が届いていません」を出す"),
				    k != nullptr && m != nullptr && k->showing_note(AudioKind::Screen) &&
					    k->note_label(AudioKind::Screen)->full_text() == dock::ui::resolve_key(keys::kDockAudioNotSharingStatus) &&
					    m->showing_note(AudioKind::Voice) &&
					    m->note_label(AudioKind::Voice)->full_text() == dock::ui::resolve_key(keys::kDockAudioNoVoiceStatus));
		results.expect_true(kGroup, QStringLiteral("(9) 共有しているが画面共有の音声がない人 (yuu) の画面の行は、その旨の文を出す"),
				    y != nullptr && y->showing_note(AudioKind::Screen) &&
					    y->note_label(AudioKind::Screen)->full_text() ==
						    dock::ui::resolve_key(keys::kDockAudioNoScreenAudioStatus));
		results.expect_true(kGroup, QStringLiteral("(9) つまみを置く行は、文を置かない"),
				    h != nullptr && !h->showing_note(AudioKind::Voice) && !h->showing_note(AudioKind::Screen));

		// 最小幅は音声タブの 1 人で広げない (参加者タブの一覧の 1 行と、メインとサブの行などで決まる)。
		const int edge = audio->parentWidget() != nullptr && audio->parentWidget()->layout() != nullptr
					 ? audio->parentWidget()->layout()->contentsMargins().left()
					 : 0;
		auto *scroll = audio_scroll(sim);
		const int bar = scroll != nullptr ? scroll->verticalScrollBar()->sizeHint().width() : 0;
		const int audio_need = audio->minimum_row_width_px() + 2 * edge + bar;
		results.expect_true(kGroup, QStringLiteral("(9) 音声タブの 1 人が必要とする幅は、ドックの最小幅を超えない"),
				    audio_need <= sim.widget()->computed_min_width(),
				    QStringLiteral("必要 %1 px、最小幅 %2 px").arg(audio_need).arg(sim.widget()->computed_min_width()));
		results.info(kGroup, QStringLiteral("(9) 音声タブの 1 人が必要とする幅 (px)"), QString::number(audio_need));
	}

	// ---- (5) 1 人の高さと、ボタンの位置 ----
	{
		auto collect = [&](std::set<int> *heights, std::set<int> *mute_right, std::set<int> *mute_left, std::set<int> *slider_x,
				   std::set<int> *name_x) {
			heights->clear();
			mute_right->clear();
			mute_left->clear();
			slider_x->clear();
			name_x->clear();
			for (const std::string &id : audio->shown_user_ids()) {
				AudioRowWidget *row = audio->row_for(id);
				if (row == nullptr)
					continue;
				heights->insert(row->height());
				for (const AudioKind kind : {AudioKind::Voice, AudioKind::Screen}) {
					mute_right->insert(row->width() - rect_in_row(row->mute(kind), row).right());
					mute_left->insert(rect_in_row(row->mute(kind), row).left());
					slider_x->insert(rect_in_row(row->slider(kind), row).left());
				}
				name_x->insert(rect_in_row(row->name_label(), row).left());
			}
		};
		std::set<int> heights, right, left, slider_x, name_x;
		collect(&heights, &right, &left, &slider_x, &name_x);
		results.expect_true(kGroup, QStringLiteral("(5) どの人も高さが同じ (共有の有無、声が届いているか、文が出ているかによらない)"), heights.size() == 1,
				    heights.empty() ? QString() : QStringLiteral("高さ %1 px、種類 %2").arg(*heights.begin()).arg(heights.size()));
		results.expect_true(kGroup, QStringLiteral("(5) ミュートのボタン、つまみ、名前の位置がどの人も同じ"),
				    right.size() == 1 && left.size() == 1 && slider_x.size() == 1 && name_x.size() == 1);
		AudioRowWidget *k = row_widget(sim, kenji);
		results.expect_true(kGroup, QStringLiteral("(5) 画面の行の文は、つまみとメーターと同じ高さを持つ"),
				    k != nullptr && k->note_label(AudioKind::Screen)->height() == k->slider(AudioKind::Screen)->parentWidget()->height());
		const int height_before = heights.empty() ? 0 : *heights.begin();

		// 状態を変えても同じ高さ: 声が届いていない人 (mei) に声が届き、共有していない人 (kenji) が画面共有の音声を持つ。
		if (ledger::PersonFact *p = person_named(sim, mei))
			p->voice_arriving = true;
		if (ledger::PersonFact *p = person_named(sim, kenji)) {
			p->sharing = true;
			p->sharing_since_us = sim.now_us() - 10'000'000;
			p->audio.has_screen_audio = true;
		}
		sim.changed();
		sim.tick_soon();
		pump(1200);
		collect(&heights, &right, &left, &slider_x, &name_x);
		AudioRowWidget *m = row_widget(sim, mei);
		results.expect_true(kGroup, QStringLiteral("(5) 状態が変わった後 (文がつまみに替わった) も、どの人も高さが同じで、変わらない"),
				    heights.size() == 1 && *heights.begin() == height_before && right.size() == 1 && slider_x.size() == 1 &&
					    m != nullptr && !m->showing_note(AudioKind::Voice) && k != nullptr && !k->showing_note(AudioKind::Screen),
				    heights.empty() ? QString() : QStringLiteral("高さ %1 px").arg(*heights.begin()));
		// 元に戻す。
		if (ledger::PersonFact *p = person_named(sim, mei))
			p->voice_arriving = false;
		if (ledger::PersonFact *p = person_named(sim, kenji)) {
			p->sharing = false;
			p->audio.has_screen_audio = false;
		}
		sim.changed();
		sim.tick_soon();
		pump(1200);
		results.expect_true(kGroup, QStringLiteral("(5) 元に戻すと、文が戻る"),
				    m != nullptr && m->showing_note(AudioKind::Voice) && k != nullptr && k->showing_note(AudioKind::Screen));
	}

	// ---- (6) ミュートのボタン ----
	{
		AudioRowWidget *h = row_widget(sim, hanako);
		seen.clear();
		results.expect_eq(kGroup, QStringLiteral("(6) 音声タブのボタンは、クリックでフォーカスを取らない設定である"),
				  click_focus_controls(h).size(), 0, click_focus_controls(h).join(QLatin1Char(' ')));
		click_center(input, h->mute(AudioKind::Voice));
		pump(1200);
		results.expect_true(kGroup, QStringLiteral("(6) ミュートのボタンをクリックしても、フォーカスを取らない"),
				    QApplication::focusWidget() != h->mute(AudioKind::Voice));
		const ActionRecord *rec = last_of(ledger::ActionId::SetVoiceMute);
		results.expect_true(kGroup, QStringLiteral("(6) 声のボタンを押す: SetVoiceMute を hanako に on=true で実行した"),
				    rec != nullptr && rec->args.user_id == user_of_name(sim, hanako) && rec->args.on && rec->ran);
		results.expect_true(kGroup, QStringLiteral("(6) 次の周期の値で、ボタンが押した状態になり、ツールチップが「ミュートを解除」になる"),
				    h->mute(AudioKind::Voice)->isChecked() &&
					    h->mute(AudioKind::Voice)->toolTip() == dock::ui::resolve_key(keys::kDockRowVoiceMuteOffA11y));
		click_center(input, h->mute(AudioKind::Voice));
		pump(1200);
		rec = last_of(ledger::ActionId::SetVoiceMute);
		results.expect_true(kGroup, QStringLiteral("(6) もう一度押す: on=false で実行し、ミュートが外れる"),
				    rec != nullptr && !rec->args.on && !h->mute(AudioKind::Voice)->isChecked());
		// 画面共有の音声のボタンを、キーボード (Space) で切り替える。
		seen.clear();
		input.activate(sim.dock());
		h->mute(AudioKind::Screen)->setFocus(Qt::TabFocusReason);
		input.key(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
		pump(1200);
		rec = last_of(ledger::ActionId::SetScreenAudioMute);
		results.expect_true(kGroup, QStringLiteral("(6) Space: 画面共有の音声のボタンが SetScreenAudioMute を on=true で実行し、押した状態になる"),
				    rec != nullptr && rec->args.on && rec->args.user_id == user_of_name(sim, hanako) && rec->ran &&
					    h->mute(AudioKind::Screen)->isChecked());
		h->mute(AudioKind::Screen)->setFocus(Qt::TabFocusReason);
		input.key(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
		pump(1200);
		results.expect_true(kGroup, QStringLiteral("(6) もう一度 Space: ミュートが外れる"), !h->mute(AudioKind::Screen)->isChecked());
		// ミュート中の画面共有の音声 (さくら) を、ポインターで解除する。
		AudioRowWidget *s = row_widget(sim, sakura);
		results.expect_true(kGroup, QStringLiteral("前提: さくらの画面共有の音声はミュート中"), s != nullptr && s->mute(AudioKind::Screen)->isChecked());
		seen.clear();
		click_center(input, s->mute(AudioKind::Screen));
		pump(1200);
		rec = last_of(ledger::ActionId::SetScreenAudioMute);
		results.expect_true(kGroup, QStringLiteral("(6) ミュート中のボタン (さくらの画面共有の音声) を押す: on=false で実行し、解除される"),
				    rec != nullptr && !rec->args.on && !s->mute(AudioKind::Screen)->isChecked());
		// 声が届いていない人 (mei) のボタンは押せず、共有していない人 (kenji) の画面のボタンは置かない。
		AudioRowWidget *m = row_widget(sim, mei);
		AudioRowWidget *k = row_widget(sim, kenji);
		seen.clear();
		click_center(input, m->mute(AudioKind::Voice));
		pump(300);
		results.expect_true(kGroup, QStringLiteral("(6) 声が届いていない人のボタンは押せず、押しても操作しない"),
				    !m->mute(AudioKind::Voice)->isEnabled() && count_of(ledger::ActionId::SetVoiceMute) == 0 &&
					    m->mute(AudioKind::Voice)->toolTip() == dock::ui::resolve_key(keys::kDockRowNoVoiceTip));
		results.expect_true(kGroup, QStringLiteral("(6) 共有していない人の画面のボタンは置かない (場所は空ける)"),
				    !k->mute(AudioKind::Screen)->isVisibleTo(k) && k->mute(AudioKind::Screen)->width() > 0 &&
					    k->mute(AudioKind::Screen)->geometry().right() == k->mute(AudioKind::Voice)->geometry().right());
		AudioRowWidget *t = row_widget(sim, taro);
		results.expect_true(kGroup, QStringLiteral("(6) メインアカウント (Taro) の声のボタンのツールチップに、既定の補足がある"),
				    t != nullptr && t->mute(AudioKind::Voice)->toolTip().contains(dock::ui::resolve_key(keys::kDockRowMainVoiceMuteTip)) &&
					    !h->mute(AudioKind::Voice)->toolTip().contains(dock::ui::resolve_key(keys::kDockRowMainVoiceMuteTip)));
	}

	// ---- (3) と (4) つまみ ----
	{
		AudioRowWidget *h = row_widget(sim, hanako);
		dock::ui::VolumeSlider *voice = h->slider(AudioKind::Voice);
		dock::ui::VolumeSlider *screen = h->slider(AudioKind::Screen);
		results.expect_true(kGroup, QStringLiteral("(4) つまみは 0 から 200、刻み 1、Page は 10"),
				    voice->minimum() == 0 && voice->maximum() == 200 && voice->singleStep() == 1 && voice->pageStep() == 10);
		results.expect_true(kGroup, QStringLiteral("(4) 現在の値 (hanako は 100% と 100%) が出る"),
				    voice->value() == 100 && screen->value() == 100 &&
					    h->value_label(AudioKind::Voice)->text() == QStringLiteral("100%"));
		results.expect_true(kGroup, QStringLiteral("(3) つまみはクリックと Tab でフォーカスを取る"),
				    voice->focusPolicy() == Qt::StrongFocus);

		// (3) クリックでフォーカスを取る (クリックの後に矢印のキーで細かく合わせられる)。
		input.activate(sim.dock());
		if (QWidget *focused = QApplication::focusWidget())
			focused->clearFocus();
		click_center(input, screen);
		pump(300);
		results.expect_true(kGroup, QStringLiteral("(3) つまみをクリックすると、フォーカスが来る"), screen->hasFocus());

		// (3) 矢印のキーはフォーカスがあるときだけ動く。
		seen.clear();
		const int before_unfocused = voice->value();
		if (QWidget *focused = QApplication::focusWidget())
			focused->clearFocus();
		input.key(Qt::Key_Right);
		input.key(Qt::Key_Right);
		results.expect_true(kGroup, QStringLiteral("(3) フォーカスがないつまみは、矢印のキーで動かない"),
				    voice->value() == before_unfocused && count_of(ledger::ActionId::SetVoiceVolume) == 0);

		// (4) キーボード: 右矢印 3 回で 103、Page Up で 113。
		voice->setFocus(Qt::TabFocusReason);
		input.key(Qt::Key_Right);
		input.key(Qt::Key_Right);
		input.key(Qt::Key_Right);
		results.expect_eq(kGroup, QStringLiteral("(4) 右矢印 3 回で 103"), voice->value(), before_unfocused + 3);
		input.key(Qt::Key_PageUp);
		results.expect_eq(kGroup, QStringLiteral("(4) Page Up で 10 増えて 113"), voice->value(), before_unfocused + 13);
		input.key(Qt::Key_Left);
		results.expect_eq(kGroup, QStringLiteral("(4) 左矢印で 1 減って 112"), voice->value(), before_unfocused + 12);
		results.expect_true(kGroup, QStringLiteral("(4) 見た目のパーセントはすぐ動く (112%)"),
				    h->value_label(AudioKind::Voice)->text() == QStringLiteral("112%"));
		// 上の矢印で 1 増え、下の矢印で 1 減る (Qt の QSlider の既定)。
		input.key(Qt::Key_Up);
		const int after_up = voice->value();
		input.key(Qt::Key_Down);
		results.expect_true(kGroup, QStringLiteral("(4) 上の矢印で 1 増えて 113、下の矢印で 1 減って 112"),
				    after_up == before_unfocused + 13 && voice->value() == before_unfocused + 12,
				    QStringLiteral("上 %1、下 %2").arg(after_up).arg(voice->value()));
		pump(300);
		const ActionRecord *rec = last_of(ledger::ActionId::SetVoiceVolume);
		results.expect_true(kGroup, QStringLiteral("(4) 動かすと SetVoiceVolume を hanako に実行する (最後の値は 112)"),
				    rec != nullptr && rec->args.volume_pct == 112 && rec->args.user_id == user_of_name(sim, hanako) && rec->ran,
				    QStringLiteral("送った数 %1").arg(count_of(ledger::ActionId::SetVoiceVolume)));
		results.expect_true(kGroup, QStringLiteral("(4) 動かす間に送る操作は、動かした回数 (7 回) より少ない (間引く)"),
				    count_of(ledger::ActionId::SetVoiceVolume) >= 1 && count_of(ledger::ActionId::SetVoiceVolume) < 7,
				    QStringLiteral("送った数 %1").arg(count_of(ledger::ActionId::SetVoiceVolume)));

		// (4) 画面共有の音声のつまみ。50 回動かす間 (イベントを処理しない) に送る操作は 1 回 (最初の値だけ)。
		seen.clear();
		screen->setValue(100);
		pump(100);
		seen.clear();
		for (int v = 101; v <= 150; ++v)
			screen->setValue(v);
		results.expect_eq(kGroup, QStringLiteral("(4) 50 回動かした間に送る操作は 1 回 (最初の値だけ)"),
				  count_of(ledger::ActionId::SetScreenAudioVolume), 1);
		pump(200);
		rec = last_of(ledger::ActionId::SetScreenAudioVolume);
		results.expect_true(kGroup, QStringLiteral("(4) 間合いの後に、まとめた最後の値 (150) を SetScreenAudioVolume で実行する"),
				    rec != nullptr && rec->args.volume_pct == 150 && rec->args.user_id == user_of_name(sim, hanako) && rec->ran);
		pump(1400);
		const ledger::dockview::AudioRowView *view = view_row_named(sim, hanako);
		results.expect_true(kGroup, QStringLiteral("(4) 次の周期の値で、View に声 112 と画面共有 150 が入り、つまみはそのまま"),
				    view != nullptr && view->voice.volume_pct == 112 && view->screen.volume_pct == 150 && voice->value() == 112 &&
					    screen->value() == 150);

		// 外からの値は、動かしていないつまみに反映する (送らない)。
		seen.clear();
		if (ledger::PersonFact *p = person_named(sim, hanako))
			p->audio.voice_volume_pct = 80;
		sim.changed();
		sim.tick_soon();
		pump(2200);
		results.expect_true(kGroup, QStringLiteral("(4) 外からの値 (80) は、動かしていないつまみに反映し、操作を送らない"),
				    voice->value() == 80 && h->value_label(AudioKind::Voice)->text() == QStringLiteral("80%") &&
					    count_of(ledger::ActionId::SetVoiceVolume) == 0);

		// (3) ダブルクリックで 100% に戻す。
		if (ledger::PersonFact *p = person_named(sim, hanako))
			p->audio.screen_volume_pct = 150;
		sim.changed();
		sim.tick_soon();
		pump(300);
		seen.clear();
		if (QWidget *focused = QApplication::focusWidget())
			focused->clearFocus();
		input.move_to(center_of(voice));
		pump(700);
		input.click();
		input.click();
		pump(1500);
		rec = last_of(ledger::ActionId::SetVoiceVolume);
		results.expect_true(kGroup, QStringLiteral("(3) ダブルクリックで 100% に戻り、SetVoiceVolume 100 を実行する"),
				    voice->value() == 100 && rec != nullptr && rec->args.volume_pct == 100 && rec->ran &&
					    h->value_label(AudioKind::Voice)->text() == QStringLiteral("100%"),
				    QStringLiteral("値 %1").arg(voice->value()));
		results.expect_true(kGroup, QStringLiteral("(3) ダブルクリックでフォーカスを取る"), voice->hasFocus());
		// Home と End で両端に動き、送る。終わったら 100% に戻す。
		seen.clear();
		input.key(Qt::Key_End);
		pump(200);
		const int at_end = voice->value();
		input.key(Qt::Key_Home);
		pump(200);
		const int at_home = voice->value();
		rec = last_of(ledger::ActionId::SetVoiceVolume);
		results.expect_true(kGroup, QStringLiteral("(4) End で 200%、Home で 0% に動き、最後の値 (0) を SetVoiceVolume で実行する"),
				    at_end == 200 && at_home == 0 && rec != nullptr && rec->args.volume_pct == 0 && rec->ran,
				    QStringLiteral("End %1 Home %2").arg(at_end).arg(at_home));
		input.click();
		input.click();
		pump(300);
		results.expect_eq(kGroup, QStringLiteral("(3) もう一度ダブルクリックで 100% に戻る"), voice->value(), 100);
		results.expect_true(kGroup, QStringLiteral("(3) 音量 0 の人 (ren) の 0% が出て、そのつまみを 100% に戻せる"),
				    row_widget(sim, ren) != nullptr && row_widget(sim, ren)->slider(AudioKind::Voice)->value() == 0 &&
					    row_widget(sim, ren)->value_label(AudioKind::Voice)->text() == QStringLiteral("0%"));
	}

	// ---- (7) メーター ----
	{
		const std::string taro_id = user_of_name(sim, taro);
		AudioRowWidget *t = row_widget(sim, taro);
		AudioRowWidget *h = row_widget(sim, hanako);
		AudioRowWidget *k = row_widget(sim, kenji);
		AudioRowWidget *r = row_widget(sim, ren);
		AudioRowWidget *s = row_widget(sim, sakura);
		// メーターを読む周期は SimPoll が持たない形のこのドックでは、値を直接渡す (周期は (1) で確認した)。
		sim.widget()->set_audio_levels(fixture_levels.tracks);
		pump(100);
		auto pixel = [](dock::ui::LevelMeter *meter, int x) {
			const QImage image = meter->grab().toImage();
			return image.pixelColor(std::clamp(x, 0, image.width() - 1), image.height() / 2);
		};
		dock::ui::LevelMeter *tm = t->meter(AudioKind::Voice);
		results.expect_true(kGroup, QStringLiteral("(7) ミュート中に話している人 (Taro) のメーターは、掛ける前の値 (-14 dB) を灰色で出す"),
				    tm->gray() && std::fabs(tm->position() - (48.0 - 14.0) / 48.0) < 0.01 &&
					    pixel(tm, 2) == tm->colors().gray);
		dock::ui::LevelMeter *hm = h->meter(AudioKind::Voice);
		results.expect_true(kGroup, QStringLiteral("(7) 音を出している人 (hanako) のメーターは、掛けた後の値 (-30 dB) を緑で出す"),
				    !hm->gray() && std::fabs(hm->position() - (48.0 - 30.0) / 48.0) < 0.01 && pixel(hm, 2) == hm->colors().green);
		dock::ui::LevelMeter *hs = h->meter(AudioKind::Screen);
		const int hs_w = hs->width();
		results.expect_true(kGroup, QStringLiteral("(7) 画面共有の音声 (-16 dB) は、-20 dB までが緑、その先が黄"),
				    !hs->gray() && pixel(hs, 2) == hs->colors().green &&
					    pixel(hs, static_cast<int>(hs_w * (28.0 / 48.0)) - 3) == hs->colors().green &&
					    pixel(hs, hs->fill_px() - 2) == hs->colors().yellow);
		dock::ui::LevelMeter *km = k->meter(AudioKind::Voice);
		const int km_w = km->width();
		results.expect_true(kGroup, QStringLiteral("(7) 音が大きい人 (kenji、-4 dB) のメーターは、緑、黄、-9 dB からの赤の順に塗る"),
				    pixel(km, 2) == km->colors().green && pixel(km, static_cast<int>(km_w * 0.70)) == km->colors().yellow &&
					    pixel(km, km->fill_px() - 2) == km->colors().red);
		results.expect_true(kGroup, QStringLiteral("(7) 音量 0 の人 (ren) のメーターは、利得が 0 なので振れず、灰色"),
				    r->meter(AudioKind::Voice)->gray() && r->meter(AudioKind::Voice)->position() == 0.0);
		// さくらの画面共有の音声: ミュート中、掛ける前 -18 dB、音量 150% (+3.52 dB)。
		results.expect_true(kGroup, QStringLiteral("(7) ミュート中の画面共有の音声 (さくら) のメーターは、掛ける前の値に音量 150% を掛けた値 (-14.5 dB) を灰色で出す"),
				    s->meter(AudioKind::Screen)->gray() &&
					    std::fabs(s->meter(AudioKind::Screen)->position() - (48.0 - 18.0 + 20.0 * std::log10(1.5)) / 48.0) < 0.01 &&
					    pixel(s->meter(AudioKind::Screen), 2) == s->meter(AudioKind::Screen)->colors().gray);
		// ミュート中のつまみを動かすと、メーターが変わる (つまみの値は次の周期のメーターの更新から反映する)。
		{
			dock::ui::VolumeSlider *ss = s->slider(AudioKind::Screen);
			ss->setValue(50);
			sim.widget()->set_audio_levels(fixture_levels.tracks);
			pump(50);
			results.expect_true(kGroup, QStringLiteral("(7) ミュート中のつまみを 50% に動かすと、灰色のメーターが -24 dB に下がる"),
					    s->meter(AudioKind::Screen)->gray() &&
						    std::fabs(s->meter(AudioKind::Screen)->position() - (48.0 - 18.0 - 20.0 * std::log10(2.0)) / 48.0) < 0.01);
			ss->setValue(150);
			ss->flush();
			pump(1200);
			sim.widget()->set_audio_levels(fixture_levels.tracks);
			pump(100);
		}
		results.expect_true(kGroup, QStringLiteral("(7) 灰色は、補足のテキストの色 (テーマの text-muted。使えなければ本文の色)"),
				    tm->colors().gray.isValid());
		// 組がない人 (mei の声、kenji の画面) は 0。
		results.expect_true(kGroup, QStringLiteral("(7) 音がない組 (声が届いていない人、画面共有の音声がない人) のメーターは 0"),
				    row_widget(sim, mei)->meter(AudioKind::Voice)->position() == 0.0 && k->meter(AudioKind::Screen)->position() == 0.0);

		// 値が変わったメーターだけを塗り直す。
		std::vector<dock::ui::LevelMeter *> all;
		for (const std::string &id : audio->shown_user_ids()) {
			AudioRowWidget *row = audio->row_for(id);
			for (const AudioKind kind : {AudioKind::Voice, AudioKind::Screen}) {
				all.push_back(row->meter(kind));
			}
		}
		std::vector<int> counts;
		for (dock::ui::LevelMeter *m : all)
			counts.push_back(m->repaint_count());
		sim.widget()->set_audio_levels(fixture_levels.tracks);
		sim.widget()->set_audio_levels(fixture_levels.tracks);
		pump(50);
		bool none_repainted = true;
		for (std::size_t i = 0; i < all.size(); ++i)
			none_repainted = none_repainted && all[i]->repaint_count() == counts[i];
		results.expect_true(kGroup, QStringLiteral("(7) 同じ値を渡し続けても、どのメーターも塗り直さない (10 回/秒でも全行を塗り直さない)"), none_repainted);
		std::vector<pipeline::AudioTrackLevel> changed = fixture_levels.tracks;
		for (pipeline::AudioTrackLevel &tr : changed) {
			if (tr.key.user_id == user_of_name(sim, hanako) && tr.key.kind == pipeline::MixerKind::Voice)
				tr.peak_after = std::pow(10.0f, -12.0f / 20.0f);
		}
		sim.widget()->set_audio_levels(changed);
		pump(50);
		int repainted = 0;
		bool only_hanako = true;
		for (std::size_t i = 0; i < all.size(); ++i) {
			if (all[i]->repaint_count() != counts[i]) {
				++repainted;
				only_hanako = only_hanako && all[i] == hm;
			}
		}
		results.expect_true(kGroup, QStringLiteral("(7) 1 人の値だけが変わると、そのメーターだけを塗り直す"), repainted == 1 && only_hanako,
				    QStringLiteral("塗り直した数 %1").arg(repainted));

		// ミュートを解除すると、掛けた後の値を色付きで出す。
		seen.clear();
		click_center(input, t->mute(AudioKind::Voice));
		pump(1200);
		std::vector<pipeline::AudioTrackLevel> unmuted = fixture_levels.tracks;
		for (pipeline::AudioTrackLevel &tr : unmuted) {
			if (tr.key.user_id == taro_id && tr.key.kind == pipeline::MixerKind::Voice) {
				tr.silenced = false;
				tr.peak_after = std::pow(10.0f, -10.0f / 20.0f);
			}
		}
		sim.widget()->set_audio_levels(unmuted);
		pump(50);
		results.expect_true(kGroup, QStringLiteral("(7) ミュートを解除した人 (Taro) のメーターは、掛けた後の値 (-10 dB) を色付きで出す"),
				    last_of(ledger::ActionId::SetVoiceMute) != nullptr && !last_of(ledger::ActionId::SetVoiceMute)->args.on &&
					    !tm->gray() && std::fabs(tm->position() - 38.0 / 48.0) < 0.01 && pixel(tm, 2) == tm->colors().green);
		// 元に戻す。
		click_center(input, t->mute(AudioKind::Voice));
		pump(1200);
		sim.widget()->set_audio_levels(fixture_levels.tracks);
	}

	// ---- (10) 名前の欄、ツールチップ、パーセントの幅、ミュート中のメーター (名前の長さ違いの場面) ----
	{
		SimPoll names(ui_preview::fixture_by_id("audio-tab-names"), options.style);
		// ドックの幅 26 em (em は基準のフォントの大きさの px。ui_preview の --widths-em と同じ計算で、ドックの枠とスクロールバーを含む)。
		const double em = dock::ui::base_point_size(names.widget()) * names.widget()->logicalDpiY() / 72.0;
		const auto dock_px = [em](double width_em) { return static_cast<int>(std::lround(width_em * em)); };
		names.dock()->setGeometry(1300, 120, dock_px(26), 900);
		names.widget()->set_tab(config::DockTab::Audio);
		names.dock()->show();
		names.start();
		pump(1500);
		dock::AudioTab *tab = names.audio();
		AudioRowWidget *harness = row_widget(names, QStringLiteral("harness-screen-1"));
		AudioRowWidget *al = row_widget(names, QStringLiteral("Al"));
		AudioRowWidget *long_en = row_widget(names, QStringLiteral("AVeryLongDisplayNameWithoutAnySpaces_2026"));
		AudioRowWidget *long_ja = row_widget(names, QStringLiteral("とても長い表示名の人がいるときの例です"));
		results.expect_true(kGroup, QStringLiteral("(10) 前提: 名前の長さ違いの人の行がある"),
				    tab != nullptr && harness != nullptr && al != nullptr && long_en != nullptr && long_ja != nullptr);
		if (tab == nullptr || harness == nullptr || al == nullptr || long_en == nullptr || long_ja == nullptr)
			return;
		const QString harness_name = QStringLiteral("harness-screen-1");
		results.expect_true(kGroup, QStringLiteral("(10) 16 文字の名前 (harness-screen-1) が、ドックの幅 26 em で省かれず、ツールチップも出さない"),
				    !harness->name_label()->is_elided() && harness->name_label()->text() == harness_name &&
					    harness->name_label()->toolTip().isEmpty(),
				    QStringLiteral("表示 %1、名前の幅 %2 px")
					    .arg(harness->name_label()->text())
					    .arg(harness->name_label()->width()));
		results.expect_true(kGroup, QStringLiteral("(10) 短い名前 (Al) も、省かず、ツールチップも出さない"),
				    !al->name_label()->is_elided() && al->name_label()->toolTip().isEmpty());
		// 長い名前は省いて、全文をツールチップに入れる。アクセシブル名も全文。
		const QString long_ja_name = long_ja->name_label()->full_text();
		results.expect_true(kGroup, QStringLiteral("(10) 省かれた名前 (長い日本語) のツールチップは全文で、アクセシブル名も全文"),
				    long_ja->name_label()->is_elided() && long_ja->name_label()->toolTip() == long_ja_name &&
					    long_ja->name_label()->accessibleName() == long_ja_name);
		results.expect_true(kGroup, QStringLiteral("(10) 省かれた名前 (長い英字) のツールチップは全文"),
				    long_en->name_label()->is_elided() &&
					    long_en->name_label()->toolTip() == long_en->name_label()->full_text());
		// 名前が長い行でも、つまみはゆとりの幅 (6 em) 以上を残す。
		const int name_w = long_en->name_label()->width();
		const int slider_w = long_en->slider(AudioKind::Voice)->width();
		results.expect_true(kGroup, QStringLiteral("(10) 名前が長い行でも、26 em ではつまみがゆとりの幅 (6 em) 以上"),
				    slider_w >= dock_px(6.0) - 1, QStringLiteral("名前 %1 px、つまみ %2 px").arg(name_w).arg(slider_w));
		results.expect_true(kGroup, QStringLiteral("(10) 名前の欄の幅とつまみの位置は、名前の長さによらずどの人も同じ"),
				    harness->name_label()->width() == al->name_label()->width() &&
					    harness->slider(AudioKind::Voice)->mapTo(harness, QPoint(0, 0)).x() ==
						    al->slider(AudioKind::Voice)->mapTo(al, QPoint(0, 0)).x() &&
					    long_ja->slider(AudioKind::Voice)->mapTo(long_ja, QPoint(0, 0)).x() ==
						    al->slider(AudioKind::Voice)->mapTo(al, QPoint(0, 0)).x());
		// パーセントの欄は、ラベルのフォントで測った 200% が欠けない幅。
		bool value_fits = true;
		for (const std::string &id : tab->shown_user_ids()) {
			AudioRowWidget *row = tab->row_for(id);
			for (const AudioKind kind : {AudioKind::Voice, AudioKind::Screen}) {
				QLabel *v = row->value_label(kind);
				const QString widest = dock::ui::resolve(ledger::TextRef{keys::kDockRowVolumeValueLabel, {"200"}});
				value_fits = value_fits && v->width() >= v->fontMetrics().horizontalAdvance(widest);
			}
		}
		results.expect_true(kGroup, QStringLiteral("(10) パーセントの欄は、ラベル自身のフォントで測った 200% が欠けない幅"), value_fits);
		// 見出し (声、画面) も、ラベル自身のフォントで測った文字の幅と、左の空き (contentsMargins) が入る幅。この場面の行は、
		// ドックを表示した後の周期で作られる (製品と同じ順)。作った直後の行はまだ polish されておらず、その時点のフォント
		// (QApplication の既定のフォント) で測ると、テーマのフォントで描く見出しより狭くなる。つまみの高さも、行のフォント
		// (polish した後のテーマのフォント) の高さにそろう。
		bool kind_fits = true;
		bool slider_follows_font = true;
		QString kind_detail;
		for (const std::string &id : tab->shown_user_ids()) {
			AudioRowWidget *row = tab->row_for(id);
			for (const AudioKind kind : {AudioKind::Voice, AudioKind::Screen}) {
				QLabel *k = row->kind_label(kind);
				const QMargins m = k->contentsMargins();
				const int need = static_cast<int>(std::ceil(QFontMetricsF(k->font()).horizontalAdvance(k->text()))) +
						 m.left() + m.right();
				if (k->width() < need) {
					kind_fits = false;
					kind_detail = QStringLiteral("%1: 幅 %2 px、必要 %3 px").arg(k->text()).arg(k->width()).arg(need);
				}
				if (row->slider(kind)->height() != std::max(row->fontMetrics().height(), 12))
					slider_follows_font = false;
			}
		}
		results.expect_true(kGroup, QStringLiteral("(10) 見出し (声、画面) は、ラベル自身のフォントで測った文字の幅と左の空きが欠けない幅"),
				    kind_fits, kind_detail);
		results.expect_true(kGroup, QStringLiteral("(10) つまみの高さは、polish した後の行のフォントの高さ"), slider_follows_font);
		// ミュート中のメーター: harness-screen-1 の声 (ミュート、-20 dB、音量 150%) は -16.5 dB、Al の画面共有の音声 (ミュート、-12 dB、
		// 音量 50%) は -18 dB を灰色で出す。
		pipeline::AudioLevels lv;
		lv.tracks = ui_preview::fixture_by_id("audio-tab-names").levels;
		names.widget()->set_audio_levels(lv.tracks);
		pump(100);
		dock::ui::LevelMeter *hm = harness->meter(AudioKind::Voice);
		dock::ui::LevelMeter *am = al->meter(AudioKind::Screen);
		results.expect_true(kGroup, QStringLiteral("(10) ミュート中 (音量 150%) のメーターは、掛ける前 (-20 dB) より 3.5 dB 大きい位置を灰色で出す"),
				    hm->gray() && std::fabs(hm->position() - (48.0 - 20.0 + 20.0 * std::log10(1.5)) / 48.0) < 0.01);
		results.expect_true(kGroup, QStringLiteral("(10) ミュート中 (音量 50%) のメーターは、掛ける前 (-12 dB) より 6 dB 小さい位置を灰色で出す"),
				    am->gray() && std::fabs(am->position() - (48.0 - 12.0 - 20.0 * std::log10(2.0)) / 48.0) < 0.01);
		// 広いドックでは、つまみをゆとりの幅 (6 em) まで広げたあと、名前の欄が全文の幅まで広がる。
		const int name_26 = long_en->name_label()->width();
		const int comfort = dock_px(6.0);
		bool slider_comfortable = true;
		int name_prev = name_26;
		bool name_grows = true;
		for (const double width_em : {32.0, 40.0}) {
			names.dock()->setGeometry(1300, 120, dock_px(width_em), 900);
			pump(400);
			slider_comfortable = slider_comfortable && long_en->slider(AudioKind::Voice)->width() >= comfort - 1;
			name_grows = name_grows && long_en->name_label()->width() >= name_prev;
			name_prev = long_en->name_label()->width();
		}
		results.expect_true(kGroup, QStringLiteral("(10) 32 em と 40 em では、つまみがゆとりの幅 (6 em) 以上で、名前の欄は広がる一方"),
				    slider_comfortable && name_grows && name_prev > name_26,
				    QStringLiteral("26 em の名前 %1 px、40 em の名前 %2 px").arg(name_26).arg(name_prev));
		results.expect_true(kGroup, QStringLiteral("(10) 40 em の名前の欄は、全員の名前の全文の幅を超えない"),
				    name_prev <= long_en->wanted_name_px());
		results.expect_true(kGroup, QStringLiteral("(10) どの幅でも、名前の欄とつまみの位置は全員同じ"),
				    harness->name_label()->width() == long_en->name_label()->width() &&
					    harness->slider(AudioKind::Voice)->mapTo(harness, QPoint(0, 0)).x() ==
						    long_en->slider(AudioKind::Voice)->mapTo(long_en, QPoint(0, 0)).x());
		// 名前の欄の幅 (全行のつまみの位置) は、並びの保留の間は変えない。マウスのボタンを押している間 (つまみを操作している間と
		// 同じ保留) に、いちばん長い 2 人の名前が短くなっても、つまみは動かず、離して止まってから動く。
		{
			Input pointer;
			pointer.activate(names.dock());
			auto slider_x = [&] { return harness->slider(AudioKind::Voice)->mapTo(harness, QPoint(0, 0)).x(); };
			const int x_before = slider_x();
			pointer.move_to(center_of(harness->name_label()));
			pointer.press();
			pump(50);
			for (const char *long_name : {"AVeryLongDisplayNameWithoutAnySpaces_2026", "とても長い表示名の人がいるときの例です"}) {
				if (ledger::PersonFact *p = person_named(names, QString::fromUtf8(long_name))) {
					p->display_name = std::string("zz");
					p->discord_name = p->display_name;
				}
			}
			names.changed();
			names.tick_soon();
			pump(1500);
			const int x_held = slider_x();
			pointer.release();
			pointer.move_to(QPoint(-50, -50));
			pump(2500);
			const int x_after = slider_x();
			results.expect_true(kGroup, QStringLiteral("(10) ボタンを押している間は、全員の名前の最大が変わっても、つまみが動かない"),
					    x_held == x_before, QStringLiteral("前 %1 px、押している間 %2 px").arg(x_before).arg(x_held));
			results.expect_true(kGroup, QStringLiteral("(10) 離して止まったら、名前の欄の幅を変える (つまみが左へ動く)"),
					    x_after < x_before, QStringLiteral("前 %1 px、後 %2 px").arg(x_before).arg(x_after));
		}
		names.stop();
		names.dock()->hide();
	}

	// ---- 接続が切れた行、合成のソースがないとき、行がないとき ----
	{
		SimPoll off(ui_preview::fixture_by_id("audio-tab-off"), options.style);
		off.dock()->setGeometry(1300, 120, 420, 900);
		off.widget()->set_tab(config::DockTab::Audio);
		off.dock()->show();
		off.start();
		pump(1500);
		dock::AudioTab *tab = off.audio();
		bool all_off = tab != nullptr && !tab->shown_user_ids().empty();
		bool buttons_off = all_off;
		if (tab != nullptr) {
			for (const std::string &id : tab->shown_user_ids()) {
				AudioRowWidget *row = tab->row_for(id);
				all_off = all_off && row != nullptr && !row->isEnabled() && row->view().off;
				buttons_off = buttons_off && row != nullptr && !row->mute(AudioKind::Voice)->isEnabled() &&
					      !row->mute(AudioKind::Screen)->isEnabled() && !row->slider(AudioKind::Voice)->isEnabled();
			}
		}
		results.expect_true(kGroup, QStringLiteral("(9) 接続が切れても行を残し、薄くして、つまみとミュートのボタンを操作できなくする"),
				    all_off && buttons_off);
		results.expect_true(kGroup, QStringLiteral("(9) 接続が切れた行の人数は、接続していたときと同じ (参加者タブの動きと同じ)"),
				    tab != nullptr && tab->shown_user_ids().size() == 8, tab != nullptr ? QString::number(tab->shown_user_ids().size()) : QString());
		off.stop();
		off.dock()->hide();

		SimPoll none(ui_preview::fixture_by_id("audio-tab-no-source"), options.style);
		none.dock()->setGeometry(1300, 120, 420, 900);
		none.widget()->set_tab(config::DockTab::Audio);
		none.dock()->show();
		none.start();
		pump(1500);
		dock::AudioTab *ntab = none.audio();
		results.expect_true(kGroup, QStringLiteral("(9) 合成のソースがない間は、行を出さず、ソースを追加すると使えることを 1 文で言う"),
				    ntab != nullptr && ntab->shown_user_ids().empty() && ntab->notice_shown() &&
					    ntab->notice_text() == dock::ui::resolve_key(keys::kDockAudioNoSourceText),
				    ntab != nullptr ? ntab->notice_text() : QString());
		none.stop();
		none.dock()->hide();

		SimPoll empty(ui_preview::fixture_by_id("audio-tab-empty"), options.style);
		empty.dock()->setGeometry(1300, 120, 420, 900);
		empty.widget()->set_tab(config::DockTab::Audio);
		empty.dock()->show();
		empty.start();
		pump(1500);
		dock::AudioTab *etab = empty.audio();
		results.expect_true(kGroup, QStringLiteral("(9) 行がない間は、行が並ぶ場所であることを 1 文で言う"),
				    etab != nullptr && etab->shown_user_ids().empty() && etab->notice_shown() &&
					    etab->notice_text() == dock::ui::resolve_key(keys::kDockAudioEmptyEmpty));
		empty.stop();
		empty.dock()->hide();
	}

	sim.stop();
	sim.dock()->hide();

	// ---- (2) ホイール、(8) ドラッグ。30 人の音声タブ ----
	{
		const ui_preview::Fixture many = ui_preview::fixture_by_id("audio-tab-30");
		SimPoll big(many, options.style);
		big.dock()->setGeometry(1300, 120, 420, 700);
		big.settings().layout_hold_idle_ms = 600;
		big.widget()->set_tab(config::DockTab::Audio);
		big.dock()->show();
		Input pointer;
		pointer.activate(big.dock());
		std::vector<ActionRecord> log;
		big.on_action = [&](const ActionRecord &rec) {
			log.push_back(rec);
			return false;
		};
		big.start();
		pump(1500);
		dock::AudioTab *tab30 = big.audio();
		QScrollArea *scroll = audio_scroll(big);
		results.expect_true(kGroup, QStringLiteral("前提: 30 人の音声タブがスクロールする"),
				    tab30 != nullptr && scroll != nullptr && scroll->verticalScrollBar()->maximum() > 0 &&
					    tab30->shown_user_ids().size() == 30);
		if (tab30 == nullptr || scroll == nullptr || tab30->shown_user_ids().size() < 20)
			return;

		// (2) つまみの上のホイール。
		AudioRowWidget *row = tab30->row_for(tab30->shown_user_ids()[1]);
		dock::ui::VolumeSlider *slider = row->slider(AudioKind::Voice);
		const int value_before = slider->value();
		const int scroll_before = scroll->verticalScrollBar()->value();
		// つまみの上へポインターを置く (先頭の近くの行で、スクロール領域の見える範囲にある)。
		pointer.move_to(center_of(slider));
		pump(50);
		for (int i = 0; i < 3; ++i) {
			pointer.wheel(-120);
			pump(30);
		}
		pump(300);
		const int scroll_after = scroll->verticalScrollBar()->value();
		const bool under_pointer = Input::widget_at(center_of(slider)) != nullptr;
		results.expect_true(kGroup, QStringLiteral("(2) つまみの上のホイールで、値が変わらない"),
				    slider->value() == value_before && under_pointer);
		results.expect_true(kGroup, QStringLiteral("(2) つまみの上のホイールで、ページがスクロールする"), scroll_after > scroll_before,
				    QStringLiteral("スクロール %1 -> %2").arg(scroll_before).arg(scroll_after));
		bool no_volume_action = true;
		for (const ActionRecord &a : log)
			no_volume_action = no_volume_action && a.id != ledger::ActionId::SetVoiceVolume;
		results.expect_true(kGroup, QStringLiteral("(2) ホイールでは音量の操作をしない"), no_volume_action);
		// フォーカスがあっても同じ。ポインターは、スクロール後のつまみの上へ動かす。
		AudioRowWidget *row2 = tab30->row_for(tab30->shown_user_ids()[0]);
		scroll->verticalScrollBar()->setValue(0);
		pump(50);
		dock::ui::VolumeSlider *first = row2->slider(AudioKind::Voice);
		first->setFocus(Qt::TabFocusReason);
		pointer.move_to(center_of(first));
		pump(50);
		const int first_value = first->value();
		const int s0 = scroll->verticalScrollBar()->value();
		pointer.wheel(-120);
		pump(200);
		results.expect_true(kGroup, QStringLiteral("(2) フォーカスのあるつまみの上でも、ホイールで値が変わらず、ページがスクロールする"),
				    first->hasFocus() && first->value() == first_value && scroll->verticalScrollBar()->value() > s0);
		first->clearFocus();
		scroll->verticalScrollBar()->setValue(0);
		pump(100);

		// (8) つまみをドラッグしている間は、行が動かない。
		AudioRowWidget *drag_row = tab30->row_for(tab30->shown_user_ids()[2]);
		const std::string drag_id = drag_row->user_id();
		dock::ui::VolumeSlider *drag = drag_row->slider(AudioKind::Voice);
		log.clear();
		pointer.move_to(QPoint(1, 1));
		pump(700);
		const QPoint handle = center_of(drag);
		pointer.move_to(handle);
		pump(900); // クリックを捨てる時間 (動いた直後のガード) が過ぎるのを待つ
		pointer.press();
		pump(50);
		// 右へドラッグする。
		for (int step = 1; step <= 6; ++step) {
			pointer.move_to(QPoint(handle.x() + step * 8, handle.y()));
			pump(30);
		}
		const int dragged = drag->value();
		results.expect_true(kGroup, QStringLiteral("前提: ドラッグでつまみが動いた"), drag->isSliderDown() && dragged > 100,
				    QStringLiteral("値 %1").arg(dragged));
		const std::vector<std::string> order_before = tab30->shown_user_ids();
		std::map<std::string, QRect> rects_before;
		for (const std::string &id : order_before)
			rects_before[id] = global_rect(tab30->row_for(id));
		// ドラッグの間に、人が抜ける、人が増える、並びが変わる、外からの値が来る。
		ledger::Observation &obs = big.obs();
		const std::string leaver = order_before[5];
		const std::string renamed = order_before[8];
		for (auto it = obs.people.begin(); it != obs.people.end(); ++it) {
			if (it->user_id == leaver) {
				obs.people.erase(it);
				break;
			}
		}
		for (ledger::PersonFact &p : obs.people) {
			if (p.user_id == renamed) {
				p.display_name = std::string("aaa-first");
				p.discord_name = p.display_name;
			}
			if (p.user_id == drag_id)
				p.audio.voice_volume_pct = 20;
		}
		ledger::PersonFact newcomer = obs.people.back();
		newcomer.user_id = "100000000000009999";
		newcomer.display_name = std::string("newcomer");
		newcomer.discord_name = newcomer.display_name;
		newcomer.slot.reset();
		newcomer.sharing = false;
		obs.people.push_back(newcomer);
		big.changed();
		big.tick_soon();
		pump(2300);
		bool same_order = tab30->shown_user_ids() == order_before;
		bool same_rects = true;
		for (const std::string &id : order_before) {
			AudioRowWidget *r = tab30->row_for(id);
			if (r == nullptr || global_rect(r) != rects_before[id])
				same_rects = false;
		}
		results.expect_true(kGroup, QStringLiteral("(8) つまみをドラッグしている間は、行の追加、削除、並べ替えを保留し、行が動かない"),
				    same_order && same_rects && tab30->has_pending_layout(),
				    QStringLiteral("順 %1、位置 %2").arg(same_order ? 1 : 0).arg(same_rects ? 1 : 0));
		results.expect_true(kGroup, QStringLiteral("(8) 抜けた人の行は、保留の間は残り、操作できなくなる"),
				    tab30->row_for(leaver) != nullptr && tab30->row_for(leaver)->departed() && !tab30->row_for(leaver)->isEnabled());
		results.expect_true(kGroup, QStringLiteral("(8) ドラッグしているつまみを、外からの値 (20) で書き換えない"),
				    drag->isSliderDown() && drag->value() == dragged);
		pointer.release();
		pump(300);
		const ActionRecord *last = nullptr;
		for (auto it = log.rbegin(); it != log.rend(); ++it) {
			if (it->id == ledger::ActionId::SetVoiceVolume) {
				last = &*it;
				break;
			}
		}
		results.expect_true(kGroup, QStringLiteral("(4) つまみを離したときに、最後の値を SetVoiceVolume で実行する"),
				    last != nullptr && last->args.volume_pct == dragged && last->args.user_id == drag_id && last->ran,
				    last != nullptr ? QStringLiteral("最後に送った値 %1、ドラッグした値 %2").arg(last->args.volume_pct).arg(dragged)
						    : QStringLiteral("(none)"));
		// ポインターが止まって layout_hold_idle_ms がたつと反映する。
		pump(1500);
		results.expect_true(kGroup, QStringLiteral("(8) 離して止まったら、保留した変化を反映する (行の追加、削除、並べ替え)"),
				    tab30->row_for(leaver) == nullptr && tab30->row_for("100000000000009999") != nullptr &&
					    tab30->shown_user_ids().size() == 30 && !tab30->has_pending_layout());
		results.expect_true(kGroup, QStringLiteral("(8) 名前を変えた人は先頭に並ぶ (メインアカウントがいないので)"),
				    !tab30->shown_user_ids().empty() && tab30->shown_user_ids().front() == renamed);

		// (4) 離したときに、まだ送っていない最後の値を送る。1 回目の動きはすぐ送り、50 ms 以内の 2 回目の動きは離すまで
		// 送らない。
		pointer.move_to(QPoint(1, 1));
		pump(700);
		dock::ui::VolumeSlider *again = tab30->row_for(drag_id)->slider(AudioKind::Voice);
		// つまみを中央 (100%) に置く。つまみの上を押さないと、溝を押した動き (ページ送り) になる。
		again->setValue(100);
		pump(1800);
		const QPoint grip = center_of(again);
		pointer.move_to(grip);
		pump(900);
		log.clear();
		const int base = again->value();
		pointer.press();
		pointer.move_to(QPoint(grip.x() + 10, grip.y()));
		pointer.move_to(QPoint(grip.x() + 24, grip.y()));
		const int after_moves = again->value();
		const int sent_before_release = static_cast<int>(
			std::count_if(log.begin(), log.end(), [](const ActionRecord &a) { return a.id == ledger::ActionId::SetVoiceVolume; }));
		pointer.release();
		const ActionRecord *flushed = nullptr;
		for (auto it = log.rbegin(); it != log.rend(); ++it) {
			if (it->id == ledger::ActionId::SetVoiceVolume) {
				flushed = &*it;
				break;
			}
		}
		results.expect_true(kGroup, QStringLiteral("(4) 離した直後に、まだ送っていなかった最後の値を送る"),
				    after_moves > base && sent_before_release == 1 && flushed != nullptr && flushed->args.volume_pct == after_moves &&
					    flushed->ran,
				    QStringLiteral("動かした後 %1、離す前に送った数 %2").arg(after_moves).arg(sent_before_release));
		big.stop();
		big.dock()->hide();
	}
}

} // namespace cordmux::dock_drive
