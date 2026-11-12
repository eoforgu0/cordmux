// SPDX-License-Identifier: Apache-2.0

// 音声タブの View (decide_dock() の DockViewV2::audio_tab と ledger/audio_tab.h) の単体テスト。行の順序、行の状態、
// 文言の選択、押せない理由、メーターが出す値。

#include "doctest.h"

#include "ledger/audio_tab.h"
#include "ledger/rules.h"
#include "ledger/text_ids.h"
#include "ledger_dock_test_helpers.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace ledger_dock_test;
using dockview::AudioRowView;
using dockview::AudioTabView;
namespace keys = cordmux::ledger::text::keys;

namespace {

// サブアカウントを使う構成。メインアカウント (500) とサブアカウント (600) が通話にいて、声が届いている。
Observation with_main_and_sub()
{
	Observation obs = healthy();
	PersonFact main = present("500", "main");
	PersonFact sub = present("600", "sub");
	sub.is_receiver = true;
	main.is_self_frame = true;
	main.voice_arriving = true;
	obs.people.push_back(main);
	obs.people.push_back(sub);
	obs.choices.broadcaster_user_id = "500";
	obs.call.roster_count = 4;
	return obs;
}

std::vector<std::string> ids_of(const AudioTabView &view)
{
	std::vector<std::string> out;
	for (const AudioRowView &row : view.rows)
		out.push_back(row.user_id);
	return out;
}

const AudioRowView *find_row(const AudioTabView &view, const std::string &user_id)
{
	for (const AudioRowView &row : view.rows) {
		if (row.user_id == user_id)
			return &row;
	}
	return nullptr;
}

PersonFact *person(Observation &obs, const std::string &user_id)
{
	for (PersonFact &p : obs.people) {
		if (p.user_id == user_id)
			return &p;
	}
	return nullptr;
}

} // namespace

TEST_CASE("音声タブは、メインアカウントを先頭に、ほかを表示名の自然順に並べる (組分けしない)")
{
	Observation obs = with_main_and_sub();
	obs.people.push_back(showing("700", "Bob2", 2));
	obs.people.push_back(showing("800", "bob10", 3));
	obs.people.push_back(present("900", "carol"));
	const AudioTabView view = decide_dock(obs).audio_tab;

	// 画面共有中の taro・Bob2・bob10 と、通話中だけの hanako・carol が、1 つの並びに自然順で入る。
	CHECK(ids_of(view) == std::vector<std::string>{"500", "700", "800", "900", "200", "100"});
	CHECK(view.rows.front().is_main);
	CHECK_FALSE(view.notice.has_value());
}

TEST_CASE("音声タブは、参加者タブと同じ人を出す。サブアカウントは出さない")
{
	Observation obs = with_main_and_sub();
	const dockview::DockViewV2 dock = decide_dock(obs);
	std::vector<std::string> people;
	if (dock.people_list.pinned.has_value())
		people.push_back(dock.people_list.pinned->user_id);
	for (const dockview::PersonRow &r : dock.people_list.sharing)
		people.push_back(r.user_id);
	for (const dockview::PersonRow &r : dock.people_list.in_call)
		people.push_back(r.user_id);
	std::vector<std::string> audio = ids_of(dock.audio_tab);
	std::sort(people.begin(), people.end());
	std::sort(audio.begin(), audio.end());
	CHECK(people == audio);
	CHECK(find_row(dock.audio_tab, "600") == nullptr);
}

TEST_CASE("メインアカウントだけの構成では、Vesktop のアカウントの行は音声タブにない (値が配信の音に反映されない)")
{
	Observation obs = healthy();
	PersonFact own = present("600", "me");
	own.is_receiver = true;
	own.is_self_frame = true;
	obs.people.push_back(own);
	const AudioTabView view = decide_dock(obs).audio_tab;
	CHECK(find_row(view, "600") == nullptr);
	CHECK(ids_of(view).size() == 2);
}

TEST_CASE("声の行は、声が届いている人はつまみを置き、まだ届いていない人は文を置く")
{
	Observation obs = with_main_and_sub();
	person(obs, "100")->voice_arriving = true;
	person(obs, "100")->audio.voice_volume_pct = 130;
	const AudioTabView view = decide_dock(obs).audio_tab;

	const AudioRowView *arrived = find_row(view, "100");
	REQUIRE(arrived != nullptr);
	CHECK(arrived->voice.control);
	CHECK(arrived->voice.note.id.empty());
	CHECK(arrived->voice.mute_shown);
	CHECK(arrived->voice.mute_enabled);
	CHECK(arrived->voice.volume_pct == 130);

	const AudioRowView *waiting = find_row(view, "200");
	REQUIRE(waiting != nullptr);
	CHECK_FALSE(waiting->voice.control);
	CHECK(waiting->voice.note.id == keys::kDockAudioNoVoiceStatus);
	// ミュートのボタンは場所を空けたまま押せない。
	CHECK(waiting->voice.mute_shown);
	CHECK_FALSE(waiting->voice.mute_enabled);
}

TEST_CASE("声をミュートした人は、声が届いていなくてもつまみを置く (ミュートを解除できる)")
{
	Observation obs = with_main_and_sub();
	person(obs, "200")->audio.voice_muted = true;
	const AudioRowView *row = find_row(decide_dock(obs).audio_tab, "200");
	REQUIRE(row != nullptr);
	CHECK(row->voice.control);
	CHECK(row->voice.muted);
	CHECK(row->voice.mute_enabled);
}

TEST_CASE("画面共有の音声の行は、持つ人につまみを置き、持たない人の文を、共有していない人と共有している人で分ける")
{
	Observation obs = with_main_and_sub();
	person(obs, "100")->audio.has_screen_audio = true;
	person(obs, "100")->audio.screen_volume_pct = 70;
	person(obs, "100")->audio.screen_muted = true;
	obs.people.push_back(showing("700", "silent", 2)); // 共有しているが、画面共有の音声がない
	const AudioTabView view = decide_dock(obs).audio_tab;

	const AudioRowView *has = find_row(view, "100");
	REQUIRE(has != nullptr);
	CHECK(has->screen.control);
	CHECK(has->screen.mute_shown);
	CHECK(has->screen.mute_enabled);
	CHECK(has->screen.muted);
	CHECK(has->screen.volume_pct == 70);

	const AudioRowView *not_sharing = find_row(view, "200");
	REQUIRE(not_sharing != nullptr);
	CHECK_FALSE(not_sharing->screen.control);
	CHECK(not_sharing->screen.note.id == keys::kDockAudioNotSharingStatus);
	CHECK_FALSE(not_sharing->screen.mute_shown);
	CHECK_FALSE(not_sharing->screen.mute_enabled);

	const AudioRowView *no_audio = find_row(view, "700");
	REQUIRE(no_audio != nullptr);
	CHECK_FALSE(no_audio->screen.control);
	CHECK(no_audio->screen.note.id == keys::kDockAudioNoScreenAudioStatus);
}

TEST_CASE("Vesktop との接続が切れても行を残し、薄くして、ミュートのボタンを押せなくする")
{
	Observation obs = with_main_and_sub();
	person(obs, "100")->voice_arriving = true;
	person(obs, "100")->audio.has_screen_audio = true;
	obs.last_known_people = obs.people;
	obs.link.state = LinkState::Disconnected;
	obs.people.clear();
	const AudioTabView view = decide_dock(obs).audio_tab;

	const AudioRowView *row = find_row(view, "100");
	REQUIRE(row != nullptr);
	CHECK(row->off);
	CHECK(row->voice.control);
	CHECK_FALSE(row->voice.mute_enabled);
	CHECK(row->screen.control);
	CHECK_FALSE(row->screen.mute_enabled);
	CHECK_FALSE(view.notice.has_value());
}

TEST_CASE("合成のソースがない間は、行を 1 つも出さず、ソースを足すと使えることを 1 文で伝える")
{
	Observation obs = with_main_and_sub();
	obs.choices.composite_present = false;
	const AudioTabView view = decide_dock(obs).audio_tab;
	CHECK(view.rows.empty());
	REQUIRE(view.notice.has_value());
	CHECK(view.notice->id == keys::kDockAudioNoSourceText);
}

TEST_CASE("合成のソースがあって参加者がいない間は、行が並ぶ場所であることを 1 文で伝える")
{
	Observation obs = not_connected();
	obs.people.clear();
	const AudioTabView view = decide_dock(obs).audio_tab;
	CHECK(view.rows.empty());
	REQUIRE(view.notice.has_value());
	CHECK(view.notice->id == keys::kDockAudioEmptyEmpty);
}

TEST_CASE("メインアカウントの行は is_main を持ち、声のミュートのツールチップの補足に使う")
{
	Observation obs = with_main_and_sub();
	const AudioTabView view = decide_dock(obs).audio_tab;
	REQUIRE(find_row(view, "500") != nullptr);
	CHECK(find_row(view, "500")->is_main);
	CHECK_FALSE(find_row(view, "200")->is_main);
}

TEST_CASE("メーターは、音を出している間は掛けた後の値を色付きで、ミュート中と音量 0 の間は掛けた後になる値を灰色で出す")
{
	// 掛ける前 -6 dBFS、掛けた後 -26 dBFS。
	const float before = std::pow(10.0f, -6.0f / 20.0f);
	const float after = std::pow(10.0f, -26.0f / 20.0f);

	const MeterReading live = meter_reading(before, after, false, 10);
	CHECK_FALSE(live.gray);
	CHECK(live.position == doctest::Approx((-26.0f + 48.0f) / 48.0f).epsilon(0.001));

	// 掛けた後は 0 になる (ミキサーが 0 にする) が、灰色のメーターは掛ける前の値に音量の利得を掛けた値を出す。
	// 音量 100% (利得 1) は掛ける前の値のまま、50% (利得 0.5) は -6 dB 下がる。
	const MeterReading muted = meter_reading(before, 0.0f, true, 100);
	CHECK(muted.gray);
	CHECK(muted.position == doctest::Approx((-6.0f + 48.0f) / 48.0f).epsilon(0.001));
	const MeterReading half = meter_reading(before, 0.0f, true, 50);
	CHECK(half.gray);
	CHECK(half.position == doctest::Approx((-6.0206f - 6.0f + 48.0f) / 48.0f).epsilon(0.001));
	// 音量 200% (利得 2) は +6 dB 上がり、範囲の外の音量は端に収まる。
	const float quiet = std::pow(10.0f, -30.0f / 20.0f);
	const float doubled = meter_reading(quiet, 0.0f, true, 200).position;
	CHECK(doubled == doctest::Approx((-30.0f + 6.0206f + 48.0f) / 48.0f).epsilon(0.001));
	CHECK(meter_reading(quiet, 0.0f, true, 500).position == doubled);
}

TEST_CASE("メーターは、ミュート中につまみを動かすと動き、音量 0 では動かない")
{
	const float before = std::pow(10.0f, -20.0f / 20.0f);
	float last = -1.0f;
	for (const int pct : {20, 60, 100, 150, 200}) {
		const float position = meter_reading(before, 0.0f, true, pct).position;
		CHECK(position > last);
		last = position;
	}
	const MeterReading zero = meter_reading(before, 0.0f, true, 0);
	CHECK(zero.gray);
	CHECK(zero.position == 0.0f);
}

TEST_CASE("メーターは、-48 dB 以下を下端、0 dB 以上を上端にし、音がなければ 0 にする")
{
	CHECK(meter_reading(0.0f, 0.0f, false, 100).position == 0.0f);
	CHECK(meter_reading(0.0f, 0.0f, true, 100).position == 0.0f);
	CHECK(meter_reading(0.0f, std::pow(10.0f, -60.0f / 20.0f), false, 100).position == 0.0f);
	CHECK(meter_reading(0.0f, 1.0f, false, 100).position == 1.0f);
	CHECK(meter_reading(0.0f, 2.0f, false, 100).position == 1.0f);
}
