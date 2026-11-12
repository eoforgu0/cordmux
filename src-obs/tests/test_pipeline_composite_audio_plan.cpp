// SPDX-License-Identifier: Apache-2.0

// pipeline/composite_audio_plan.h と pipeline/mixer_roster.h の単体テスト。合成のソースが出す音を置いてよいか
// (合成のソースの有無とダミー表示)、置く時刻 (送信側のタイムスタンプの変換、画面の音の D と半刻み、声の J と声の遅延)、
// ミキサーの組の決め方、組の出入り (取り上げ、経路の削除、使う声)、参加者ごとの利得の表を確認する。

#include "doctest.h"

#include "config/seen_people.h"
#include "config/settings.h"
#include "ledger/observation.h"
#include "pipeline/audio_mixer.h"
#include "pipeline/composite_audio_plan.h"
#include "pipeline/mixer_roster.h"
#include "pipeline/sender_clock.h"
#include "pipeline/stream_router.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <vector>

using cordmux::config::AllocationTable;
using cordmux::config::SlotKind;
using cordmux::pipeline::AssignPolicy;
using cordmux::pipeline::desired_mixer_keys;
using cordmux::pipeline::kVoiceMixJitterUs;
using cordmux::pipeline::MixerKey;
using cordmux::pipeline::MixerKind;
using cordmux::pipeline::MixerLevels;
using cordmux::pipeline::MixerRoster;
using cordmux::pipeline::Route;
using cordmux::pipeline::StreamRouter;
using cordmux::ws::ConfigureMessage;
using cordmux::ws::StreamConfig;
using cordmux::ws::StreamKind;

namespace {

StreamConfig make_stream(std::uint32_t stream_id, const std::string &user_id, StreamKind kind)
{
	StreamConfig c;
	c.stream_id = stream_id;
	c.user_id = user_id;
	c.kind = kind;
	c.audio = true;
	return c;
}

MixerKey screen_key(const char *user) { return MixerKey{user, MixerKind::ScreenAudio}; }
MixerKey voice_key(const char *user) { return MixerKey{user, MixerKind::Voice}; }

bool has(const std::vector<MixerKey> &keys, const MixerKey &key)
{
	for (const MixerKey &k : keys) {
		if (k == key)
			return true;
	}
	return false;
}

const AssignPolicy kAuto{true, {}};

} // namespace

TEST_CASE("送信側のタイムスタンプは、接続ごとに決めた時計の差を足して OBS の時計に変換される")
{
	using cordmux::pipeline::sender_to_obs_us;
	// 差 (OBS の時計 - 送信側の時計) が -4800000 なら、送信側の 4990000 は OBS の 190000。
	CHECK(sender_to_obs_us(4990000, std::optional<std::int64_t>(-4800000), 777) == 190000);
	// 差がまだ決まっていなければ、今届いたものとして OBS の今を返す。
	CHECK(sender_to_obs_us(4990000, std::nullopt, 777) == 777);
}

TEST_CASE("画面の音の時刻は、OBS の時計に換算した時刻に D と半刻みを足す")
{
	// 17 ms 前に生まれた音 (OBS の時計に換算した時刻 = OBS の今 - 17000)。D = 100 ms、刻み 16667 us。
	const std::int64_t obs = 300000;
	const std::int64_t at = cordmux::pipeline::screen_audio_at_us(obs - 17000, obs, 100000, 16667, 0);
	CHECK(at == obs - 17000 + 100000 + 8333);
}

TEST_CASE("画面の音の半刻みは刻みの間隔の半分で、刻みが分からなければ足さない")
{
	CHECK(cordmux::pipeline::screen_audio_half_tick_us(16667) == 8333);
	CHECK(cordmux::pipeline::screen_audio_half_tick_us(33333) == 16666);
	CHECK(cordmux::pipeline::screen_audio_half_tick_us(0) == 0);
	CHECK(cordmux::pipeline::screen_audio_half_tick_us(-5) == 0);
}

TEST_CASE("画面の音の時刻は、D が 0 でも半刻みを足し、負の D は 0 として扱う (下限より後ならそのまま)")
{
	// 換算した時刻が OBS の今より J 先の音は、D = 0 でも半刻みを足した時刻になる。
	const std::int64_t obs = 500;
	const std::int64_t mapped = obs + kVoiceMixJitterUs;
	CHECK(cordmux::pipeline::screen_audio_at_us(mapped, obs, 0, 16667, 0) == obs + kVoiceMixJitterUs + 8333);
	CHECK(cordmux::pipeline::screen_audio_at_us(mapped, obs, -5, 16667, 0) == obs + kVoiceMixJitterUs + 8333);
}

TEST_CASE("画面の音の時刻は、OBS の今 + J より前にしない")
{
	const std::int64_t obs = 300000;
	const std::int64_t floor_us = obs + kVoiceMixJitterUs;
	// D = 0: 17 ms 前に生まれた音の計算した時刻 (obs - 17000 + 8333) は下限より前なので、下限に置く。
	CHECK(cordmux::pipeline::screen_audio_at_us(obs - 17000, obs, 0, 16667, 0) == floor_us);
	// 刻みが分からなくても、負の補正が大きくても、下限より前にならない。
	CHECK(cordmux::pipeline::screen_audio_at_us(obs - 17000, obs, 0, 0, 30000) == floor_us);
	// D = 100 ms: 計算した時刻 (obs - 17000 + 100000 + 8333) は下限より後なので、そのまま使う。
	CHECK(cordmux::pipeline::screen_audio_at_us(obs - 17000, obs, 100000, 16667, 0) == obs - 17000 + 100000 + 8333);
	// 下限の境目: 計算した時刻がちょうど下限なら、どちらでも同じ。
	CHECK(cordmux::pipeline::screen_audio_at_us(obs + kVoiceMixJitterUs, obs, 0, 0, 0) == floor_us);
	// 下限の分だけ映像より遅れる。ずれは D が小さいほど大きく、D = 0 で J - 半刻み ほどになる。
	const std::int64_t video_at = obs - 17000 + 0 + 8333;
	CHECK(cordmux::pipeline::screen_audio_at_us(obs - 17000, obs, 0, 16667, 0) - video_at ==
	      kVoiceMixJitterUs + 17000 - 8333);
}

TEST_CASE("画面の音と映像のずれの負の補正は、音を早める")
{
	// 映像のタイムスタンプに足す分 (early) だけ、音の時刻が早くなる。
	// (下限より後になる D = 100 ms で比べる。)
	const std::int64_t base = cordmux::pipeline::screen_audio_at_us(500, 500, 100000, 16667, 0);
	CHECK(base > 500 + kVoiceMixJitterUs + 30000);
	CHECK(cordmux::pipeline::screen_audio_at_us(500, 500, 100000, 16667, 30000) == base - 30000);
}

TEST_CASE("声の時刻は、届いた時の OBS の時計に J と声の遅延を足し、D では遅らせない")
{
	CHECK(cordmux::pipeline::voice_audio_at_us(700000, 0) == 700000 + kVoiceMixJitterUs);
	CHECK(cordmux::pipeline::voice_audio_at_us(700000, 30000) == 700000 + kVoiceMixJitterUs + 30000);
	CHECK(cordmux::pipeline::voice_audio_at_us(700000, -1) == 700000 + kVoiceMixJitterUs);
}

TEST_CASE("J は AudioMixer の先渡しの幅以上で、先渡しの幅に届き方の揺れの余裕 10 ms を足した値である")
{
	CHECK(kVoiceMixJitterUs >= cordmux::pipeline::AudioMixerConfig{}.lookahead_us);
	CHECK(kVoiceMixJitterUs == cordmux::pipeline::kMixerLookaheadUs + 10000);
	CHECK(kVoiceMixJitterUs == 65000);
}

TEST_CASE("組は、画面の音なら画面共有の人、声なら枠に載っている人で決まり、枠の外は組を持たない")
{
	CHECK(*cordmux::pipeline::mixer_key_for(Route{SlotKind::Screen, 2, "alice"}, "alice") == screen_key("alice"));
	CHECK(*cordmux::pipeline::mixer_key_for(Route{SlotKind::Voice, 1, "sub"}, "taro") == voice_key("taro"));
	// 枠に載っている人が分からなければ経路の人にする。
	CHECK(*cordmux::pipeline::mixer_key_for(Route{SlotKind::Voice, 1, "sub"}, "") == voice_key("sub"));
	// 枠から外した経路 (枠 0) は組を持たない。
	CHECK_FALSE(cordmux::pipeline::mixer_key_for(Route{SlotKind::Screen, 0, "alice"}, "alice").has_value());
	CHECK_FALSE(cordmux::pipeline::mixer_key_for(Route{SlotKind::Voice, 0, "sub"}, "").has_value());
	// 全画面だけで映す人の経路も組を持たない (画面共有の音は出さない)。
	CHECK_FALSE(cordmux::pipeline::mixer_key_for(Route{SlotKind::Screen, cordmux::config::kFocusOnlySlot, "eve"}, "")
			    .has_value());
}

TEST_CASE("名簿は、組が入ったことと、どのストリームも使わなくなったことを伝える")
{
	MixerRoster roster;
	auto change = roster.set_stream(1, screen_key("alice"));
	REQUIRE(change.added.size() == 1);
	CHECK(change.added[0] == screen_key("alice"));
	CHECK(change.removed.empty());
	CHECK(roster.contains(screen_key("alice")));

	// 同じ組を続けて設定しても、何も起きない。
	CHECK(roster.set_stream(1, screen_key("alice")).empty());

	// 同じ組を 2 つのストリームが使うときは、片方をやめても外れない。
	CHECK(roster.set_stream(2, screen_key("alice")).empty());
	CHECK(roster.set_stream(1, std::nullopt).empty());
	CHECK(roster.contains(screen_key("alice")));
	change = roster.set_stream(2, std::nullopt);
	REQUIRE(change.removed.size() == 1);
	CHECK(change.removed[0] == screen_key("alice"));
	CHECK_FALSE(roster.contains(screen_key("alice")));
}

TEST_CASE("枠の人が書き換わると、前の人の組が外れて新しい人の組が入る")
{
	MixerRoster roster;
	roster.set_stream(1, screen_key("alice"));
	const auto change = roster.set_stream(1, screen_key("bob"));
	REQUIRE(change.removed.size() == 1);
	REQUIRE(change.added.size() == 1);
	CHECK(change.removed[0] == screen_key("alice"));
	CHECK(change.added[0] == screen_key("bob"));
}

TEST_CASE("名簿の種類ごとの一覧と、全部外す動作")
{
	MixerRoster roster;
	roster.set_stream(1, screen_key("alice"));
	roster.set_stream(2, voice_key("alice"));
	roster.set_stream(3, screen_key("bob"));
	CHECK(roster.keys(MixerKind::ScreenAudio).size() == 2);
	CHECK(roster.keys(MixerKind::Voice).size() == 1);
	const auto change = roster.clear();
	CHECK(change.removed.size() == 3);
	CHECK(roster.keys(MixerKind::ScreenAudio).empty());
}

TEST_CASE("configure で画面共有をやめた人の組は、名簿を作り直すと外れる")
{
	AllocationTable table;
	StreamRouter router;
	MixerRoster roster;

	ConfigureMessage first;
	first.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	first.streams.push_back(make_stream(2, "bob", StreamKind::Screen));
	router.apply_configure(first, table, kAuto);
	auto change = roster.sync(desired_mixer_keys(router, table));
	CHECK(change.added.size() == 2);

	ConfigureMessage second;
	second.streams.push_back(make_stream(2, "bob", StreamKind::Screen));
	router.apply_configure(second, table, kAuto);
	change = roster.sync(desired_mixer_keys(router, table));
	REQUIRE(change.removed.size() == 1);
	CHECK(change.removed[0] == screen_key("alice"));
	CHECK(change.added.empty());
}

TEST_CASE("枠を空けられた人の組は外れ、空いた枠に載った人の組が入る")
{
	AllocationTable table;
	StreamRouter router;
	MixerRoster roster;

	ConfigureMessage first;
	for (std::uint32_t i = 1; i <= 4; ++i)
		first.streams.push_back(make_stream(i, std::string("p") + std::to_string(i), StreamKind::Screen));
	router.apply_configure(first, table, kAuto);
	CHECK(roster.sync(desired_mixer_keys(router, table)).added.size() == 4);

	// p1 が画面共有をやめて枠 1 が空き (pipeline/slot_release.h)、新しい人 p5 が空いた枠 1 に載る。
	table.release(SlotKind::Screen, 1);
	ConfigureMessage second;
	for (std::uint32_t i = 2; i <= 4; ++i)
		second.streams.push_back(make_stream(i, std::string("p") + std::to_string(i), StreamKind::Screen));
	second.streams.push_back(make_stream(5, "p5", StreamKind::Screen));
	router.apply_configure(second, table, kAuto);

	const auto change = roster.sync(desired_mixer_keys(router, table));
	REQUIRE(change.removed.size() == 1);
	REQUIRE(change.added.size() == 1);
	CHECK(change.removed[0] == screen_key("p1"));
	CHECK(change.added[0] == screen_key("p5"));
}

TEST_CASE("使う声は、枠に載っている人の組に入る")
{
	AllocationTable table;
	StreamRouter router;
	MixerRoster roster;

	// 枠に載っている人 taro の声を、サブアカウント sub の声に置き換える。
	AssignPolicy policy = kAuto;
	policy.voice_links = {{"taro", "sub"}};
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(10, "taro", StreamKind::Voice));
	msg.streams.push_back(make_stream(11, "sub", StreamKind::Voice));
	router.apply_configure(msg, table, policy);

	const auto keys = desired_mixer_keys(router, table);
	// taro 自身の声の経路はなく、sub の声が taro の組に入る。
	CHECK(keys.count(10) == 0);
	REQUIRE(keys.count(11) == 1);
	CHECK(keys.at(11) == voice_key("taro"));
}

TEST_CASE("通話に参加していないメインアカウントに使う声を結ぶと、その声はメインアカウントの声の枠と組に入る")
{
	// settings.json と seen_people.json の中身から、ドック (dock/dock_poll.cpp の push_assign_policy) と同じ材料で
	// 決め方を作る。メインアカウントは通話に参加していないので、自分のストリームを 1 つも持たない。
	cordmux::config::Settings settings;
	std::string error;
	REQUIRE(cordmux::config::parse_settings(R"({"assign_mode": "auto", "broadcaster_user_id": "main"})", &settings,
						&error));
	std::vector<cordmux::ledger::SeenPerson> seen;
	REQUIRE(cordmux::config::seen_people_from_json(
		R"([{"user_id": "main", "voice_source_user_id": "voice-a"},
		    {"user_id": "sharer", "voice_source_user_id": "voice-b"}])",
		&seen, &error));
	AssignPolicy policy;
	policy.automatic = settings.assign_mode == cordmux::config::AssignMode::Auto;
	policy.voice_links = cordmux::ledger::voice_source_marks(seen);
	policy.main_account_user_id = settings.broadcaster_user_id;

	// 通話に参加している人は、pipeline.cpp の handle_configure と同じく configure に載っている人である。
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "sharer", StreamKind::Screen));
	msg.streams.push_back(make_stream(2, "voice-a", StreamKind::Voice));
	msg.streams.push_back(make_stream(3, "voice-b", StreamKind::Voice));
	std::set<std::string> in_call;
	for (const StreamConfig &stream : msg.streams)
		in_call.insert(stream.user_id);

	AllocationTable table;
	StreamRouter router;
	router.apply_configure(msg, table, policy);

	// voice-a の声は、通話に参加していない main の声の枠に入る。経路の user_id は声を送っている人のまま。
	REQUIRE(router.route(2).has_value());
	CHECK(router.route(2)->kind == SlotKind::Voice);
	CHECK(router.route(2)->user_id == "voice-a");
	const auto *main_slot = table.assignment(SlotKind::Voice, router.route(2)->slot);
	REQUIRE(main_slot != nullptr);
	CHECK(main_slot->owner_user_id == "main");
	CHECK(main_slot->voice_user_id == "voice-a");
	CHECK(router.output_allowed(2, table));
	// voice-b の声は、画面を共有している sharer の声の枠に入る。
	REQUIRE(router.route(3).has_value());
	const auto *sharer_slot = table.assignment(SlotKind::Voice, router.route(3)->slot);
	REQUIRE(sharer_slot != nullptr);
	CHECK(sharer_slot->owner_user_id == "sharer");
	CHECK(router.output_allowed(3, table));

	// ミキサーの組は声の枠に載っている人になるので、話しているかの表 (合成のソースの光) も main と sharer で引ける。
	const auto keys = desired_mixer_keys(router, table);
	REQUIRE(keys.count(2) == 1);
	CHECK(keys.at(2) == voice_key("main"));
	REQUIRE(keys.count(3) == 1);
	CHECK(keys.at(3) == voice_key("sharer"));
	REQUIRE(keys.count(1) == 1);
	CHECK(keys.at(1) == screen_key("sharer"));
}

TEST_CASE("枠から外した人の経路は組を持たない")
{
	AllocationTable table;
	StreamRouter router;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "alice", StreamKind::Screen));
	router.apply_configure(msg, table, kAuto);
	CHECK(desired_mixer_keys(router, table).size() == 1);

	router.retarget_user("alice", SlotKind::Screen, 0);
	CHECK(desired_mixer_keys(router, table).empty());
}

TEST_CASE("参加者ごとの音量は既定が 100% でミュートなしで、画面の音と声は別に持つ")
{
	MixerLevels levels;
	CHECK(levels.get(voice_key("alice")).gain() == 1.0f);
	CHECK(levels.get(voice_key("alice")).volume_pct == 100);
	CHECK_FALSE(levels.get(voice_key("alice")).muted);

	levels.set_volume_pct(voice_key("alice"), 50);
	levels.set_mute(screen_key("alice"), true);
	CHECK(levels.get(voice_key("alice")).gain() == 0.5f);
	CHECK_FALSE(levels.get(voice_key("alice")).muted);
	CHECK(levels.get(screen_key("alice")).gain() == 1.0f);
	CHECK(levels.get(screen_key("alice")).muted);
	// ほかの人には影響しない。
	CHECK(levels.get(voice_key("bob")).gain() == 1.0f);
}

TEST_CASE("自分の声の組だけは既定でミュートで、話しているかを利得とミュートを掛ける前の音で判定する")
{
	MixerLevels levels;
	CHECK(levels.self_voice_user().empty());
	CHECK_FALSE(levels.get(voice_key("main")).muted);

	CHECK(levels.set_self_voice_user("main"));
	CHECK_FALSE(levels.set_self_voice_user("main")); // 変わらなければ偽
	CHECK(levels.get(voice_key("main")).muted);
	CHECK(levels.get(voice_key("main")).gain() == 1.0f);
	CHECK(levels.activity_before_gain(voice_key("main")));
	// 同じ人の画面の音と、ほかの人の声は既定のまま。
	CHECK_FALSE(levels.get(screen_key("main")).muted);
	CHECK_FALSE(levels.activity_before_gain(screen_key("main")));
	CHECK_FALSE(levels.get(voice_key("friend")).muted);
	CHECK_FALSE(levels.activity_before_gain(voice_key("friend")));
}

TEST_CASE("自分の声の音量だけを変えても既定のミュートは外れず、ミュートを外せば外れたまま残る")
{
	MixerLevels levels;
	levels.set_self_voice_user("main");
	levels.set_volume_pct(voice_key("main"), 50);
	CHECK(levels.get(voice_key("main")).muted);
	CHECK(levels.get(voice_key("main")).gain() == 0.5f);

	levels.set_mute(voice_key("main"), false);
	CHECK_FALSE(levels.get(voice_key("main")).muted);
	CHECK(levels.activity_before_gain(voice_key("main"))); // 判定の仕方はミュートに依らない

	// 自分の声の人が替わると、前の人は普通の人になる。新しい人の声には、自分の声のミュートの同じ値が当たる
	// (ここでは、外した値)。
	levels.set_self_voice_user("other");
	CHECK_FALSE(levels.get(voice_key("main")).muted);
	CHECK_FALSE(levels.activity_before_gain(voice_key("main")));
	CHECK_FALSE(levels.get(voice_key("other")).muted);
	levels.set_mute(voice_key("other"), true);
	CHECK(levels.get(voice_key("other")).muted);

	// 空にすると、誰の声も既定でミュートにしない。
	levels.set_self_voice_user("");
	CHECK_FALSE(levels.get(voice_key("other")).muted);
	CHECK_FALSE(levels.activity_before_gain(MixerKey{"", MixerKind::Voice}));
}

TEST_CASE("合成のソースで自分の声に経路を作っても、既定では配信の音に混ざらず、話していることは分かる")
{
	// 合成のソースがあるときの決め方で、メインアカウント main と friend の声が届く。
	AllocationTable table;
	StreamRouter router;
	AssignPolicy policy = kAuto;
	policy.main_account_user_id = "main";
	policy.route_main_account_voice = true;
	ConfigureMessage msg;
	msg.streams.push_back(make_stream(1, "main", StreamKind::Voice));
	msg.streams.push_back(make_stream(2, "friend", StreamKind::Voice));
	router.apply_configure(msg, table, policy);

	// pipeline.cpp と同じく、名簿に入った組に表の値を当てる。
	MixerLevels levels;
	levels.set_self_voice_user("main");
	MixerRoster roster;
	cordmux::pipeline::AudioMixerConfig config;
	config.lookahead_us = 20000;
	cordmux::pipeline::AudioMixer mixer(config);
	const MixerRoster::Change change = roster.sync(desired_mixer_keys(router, table));
	REQUIRE(change.added.size() == 2);
	for (const MixerKey &key : change.added)
		cordmux::pipeline::apply_mixer_level(mixer, levels, key);

	// main の声だけが届いた区間。
	constexpr std::int64_t kT = 1000000;
	std::vector<float> chunk(480 * 2, 0.3f);
	mixer.add(kT, voice_key("main"), kT, chunk.data(), 480);
	std::vector<cordmux::pipeline::MixedBlock> blocks = mixer.take(kT);
	REQUIRE_FALSE(blocks.empty());
	for (const float sample : blocks[0].samples)
		CHECK(sample == 0.0f); // 配信の音 (ミキサーの出力) に混ざらない
	CHECK(blocks[0].audible_voices == std::vector<std::string>{"main"}); // 自分の枠は光る

	// friend の声は今までどおり鳴る。
	mixer.add(kT, voice_key("friend"), kT + 20000, chunk.data(), 480);
	blocks = mixer.take(kT + 20000);
	bool friend_sounded = false;
	for (const cordmux::pipeline::MixedBlock &block : blocks) {
		if (block.audible_voices == std::vector<std::string>{"friend"})
			friend_sounded = block.samples[0] != 0.0f;
	}
	CHECK(friend_sounded);

	// 利用者が自分の声のミュートを外すと鳴る (Discord を通った声を配信に出す使い方)。次の声は、前の声の続きに置かない
	// (続きに置く許容幅の 70 ms より後に話し始めた) ものにする。
	levels.set_mute(voice_key("main"), false);
	cordmux::pipeline::apply_mixer_level(mixer, levels, voice_key("main"));
	mixer.add(kT, voice_key("main"), kT + 100000, chunk.data(), 480);
	blocks = mixer.take(kT + 100000);
	bool main_sounded = false;
	for (const cordmux::pipeline::MixedBlock &block : blocks) {
		if (block.audible_voices == std::vector<std::string>{"main"} && block.samples[0] != 0.0f)
			main_sounded = true;
	}
	CHECK(main_sounded);
}

TEST_CASE("範囲 (0 から 200) の外の音量は端に収める")
{
	MixerLevels levels;
	levels.set_volume_pct(voice_key("alice"), -5);
	CHECK(levels.get(voice_key("alice")).volume_pct == 0);
	CHECK(levels.get(voice_key("alice")).gain() == 0.0f);
	levels.set_volume_pct(voice_key("alice"), 1000);
	CHECK(levels.get(voice_key("alice")).volume_pct == 200);
	CHECK(levels.get(voice_key("alice")).gain() == 2.0f);
}

TEST_CASE("合成のソースがあり、ダミー表示の間でなければ、音を AudioMixer に置き、合成のソースがない間は置かない")
{
	CHECK(cordmux::pipeline::mixed_audio_admitted(/*composite_present=*/true, /*dummy_mode=*/false));
	CHECK_FALSE(cordmux::pipeline::mixed_audio_admitted(/*composite_present=*/false, /*dummy_mode=*/false));
}

TEST_CASE("ダミー表示の間は、合成のソースがあっても実際の画面共有の音と声を置かない")
{
	CHECK_FALSE(cordmux::pipeline::mixed_audio_admitted(/*composite_present=*/true, /*dummy_mode=*/true));
	CHECK_FALSE(cordmux::pipeline::mixed_audio_admitted(/*composite_present=*/false, /*dummy_mode=*/true));
}
