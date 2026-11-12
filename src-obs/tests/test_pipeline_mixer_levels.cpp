// SPDX-License-Identifier: Apache-2.0

// pipeline/mixer_roster.h の MixerLevels (参加者ごとの音量とミュート) と ScreenShareTracker (画面共有の始まり) の単体テスト。
// 既定の値、自分の声のミュートを持つ場所、画面共有の音が共有のたびにミュートから始まること、つなぎ直しを共有の続きと
// 扱うこと、AudioMixer に当たる利得を確認する。

#include "doctest.h"

#include "config/person_audio.h"
#include "pipeline/mixer_roster.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

using cordmux::pipeline::apply_mixer_level;
using cordmux::pipeline::kShareResumeWindowUs;
using cordmux::pipeline::MixerKey;
using cordmux::pipeline::MixerKind;
using cordmux::pipeline::MixerLevels;
using cordmux::pipeline::ScreenShareTracker;
using cordmux::pipeline::screen_sharers_of;

namespace {

MixerKey voice_key(const char *user) { return MixerKey{user, MixerKind::Voice}; }
MixerKey screen_key(const char *user) { return MixerKey{user, MixerKind::ScreenAudio}; }

constexpr std::int64_t kSec = 1000000;

} // namespace

TEST_CASE("音量の範囲は 0 から 200 で、利得は音量を 100 で割った値になる")
{
	using namespace cordmux::config;
	CHECK(kPersonVolumeMinPct == 0);
	CHECK(kPersonVolumeMaxPct == 200);
	CHECK(kPersonVolumeDefaultPct == 100);
	CHECK(gain_of_person_volume(0) == 0.0f);
	CHECK(gain_of_person_volume(100) == 1.0f);
	CHECK(gain_of_person_volume(200) == 2.0f);
	CHECK(gain_of_person_volume(37) == doctest::Approx(0.37f));
	// 範囲の外は端に収める。
	CHECK(gain_of_person_volume(-1) == 0.0f);
	CHECK(gain_of_person_volume(201) == 2.0f);
	CHECK(person_volume_in_range(0));
	CHECK(person_volume_in_range(200));
	CHECK_FALSE(person_volume_in_range(-1));
	CHECK_FALSE(person_volume_in_range(201));
}

TEST_CASE("ほかの人の声は既定でミュートなし 100% で、画面共有の音の始まりのミュートは音量を変えない")
{
	MixerLevels levels;
	CHECK_FALSE(levels.get(voice_key("friend")).muted);
	CHECK(levels.get(voice_key("friend")).volume_pct == 100);

	levels.set_volume_pct(screen_key("friend"), 150);
	levels.mute_for_share_start("friend");
	CHECK(levels.get(screen_key("friend")).muted);
	CHECK(levels.get(screen_key("friend")).volume_pct == 150);
	// 声には影響しない。
	CHECK_FALSE(levels.get(voice_key("friend")).muted);

	// 利用者がミュートを外すと、外れたまま残る (次の共有の始まりまで)。
	levels.set_mute(screen_key("friend"), false);
	CHECK_FALSE(levels.get(screen_key("friend")).muted);
	CHECK(levels.get(screen_key("friend")).volume_pct == 150);
}

TEST_CASE("自分の声のミュートは参加者ごとの値ではなく 1 つで、既定はミュートで、メインアカウントが替わっても同じ値が当たる")
{
	MixerLevels levels;
	CHECK(levels.self_voice_muted()); // 既定
	levels.set_self_voice_user("main");
	CHECK(levels.get(voice_key("main")).muted);

	// 覚えていた値 (ミュートなし) を渡すと、自分の声の組はその値になる。変わらなければ偽を返す。
	CHECK(levels.set_self_voice_muted(false));
	CHECK_FALSE(levels.set_self_voice_muted(false));
	CHECK_FALSE(levels.get(voice_key("main")).muted);

	// 利用者が人の行でミュートしても、自分の声の値として入る。
	levels.set_mute(voice_key("main"), true);
	CHECK(levels.self_voice_muted());

	// 新しいメインアカウントの声に、同じ値が当たる。前の人はふつうの人に戻る (ミュートなし)。
	levels.set_self_voice_user("other");
	CHECK(levels.get(voice_key("other")).muted);
	CHECK_FALSE(levels.get(voice_key("main")).muted);

	// 自分の声の人が前から参加者ごとの値を持っていても (ふつうの人だった間に変えたミュート)、自分の声の値が勝つ。
	MixerLevels reused;
	reused.set_mute(voice_key("main"), true); // まだ自分の声の人ではない間の値
	reused.set_self_voice_user("main");
	reused.set_self_voice_muted(false);
	CHECK_FALSE(reused.get(voice_key("main")).muted);
	// 自分の声でなくなれば、その人の値に戻る。
	reused.set_self_voice_user("");
	CHECK(reused.get(voice_key("main")).muted);

	// 自分の声の音量は、ほかの人と同じく参加者ごとの値である。
	MixerLevels volume;
	volume.set_self_voice_user("main");
	volume.set_volume_pct(voice_key("main"), 40);
	CHECK(volume.get(voice_key("main")).volume_pct == 40);
	CHECK(volume.get(voice_key("main")).muted);
}

TEST_CASE("値を持っている人は user_ids に入り、自分の声の人も入る")
{
	MixerLevels levels;
	CHECK(levels.user_ids().empty());
	levels.set_volume_pct(voice_key("a"), 80);
	levels.mute_for_share_start("b");
	levels.set_self_voice_user("main");
	CHECK(levels.user_ids() == std::set<std::string>{"a", "b", "main"});
}

TEST_CASE("AudioMixer に当たる利得は音量の割合で、ミュートは別に当たる")
{
	cordmux::pipeline::AudioMixerConfig config;
	config.lookahead_us = 20000;
	cordmux::pipeline::AudioMixer mixer(config);
	MixerLevels levels;
	levels.set_volume_pct(voice_key("a"), 50);
	levels.set_mute(voice_key("a"), true);
	apply_mixer_level(mixer, levels, voice_key("a"));

	constexpr std::int64_t kT = 1000000;
	std::vector<float> chunk(480 * 2, 0.4f);
	mixer.add(kT, voice_key("a"), kT, chunk.data(), 480);
	std::vector<cordmux::pipeline::MixedBlock> blocks = mixer.take(kT);
	REQUIRE_FALSE(blocks.empty());
	for (const float sample : blocks[0].samples)
		CHECK(sample == 0.0f); // ミュートで鳴らない

	levels.set_mute(voice_key("a"), false);
	apply_mixer_level(mixer, levels, voice_key("a"));
	mixer.add(kT, voice_key("a"), kT + 100000, chunk.data(), 480);
	blocks = mixer.take(kT + 100000);
	bool sounded = false;
	for (const cordmux::pipeline::MixedBlock &block : blocks) {
		if (block.samples[0] != 0.0f) {
			// 0.4 の 50% を超えない (ミュートを外した直後は直線で移るので、ちょうどの値は見ない)。
			CHECK(std::fabs(block.samples[0]) <= 0.2f + 1e-4f);
			sounded = true;
		}
	}
	CHECK(sounded);
}

TEST_CASE("共有を新しく始めた人だけが始まりとして返り、続いている間は返らない")
{
	ScreenShareTracker tracker;
	CHECK(tracker.update({}, 0).empty());
	CHECK(tracker.update({"a"}, 1 * kSec) == std::vector<std::string>{"a"});
	// 同じ人が続いている間 (configure が来るたび) は返らない。
	CHECK(tracker.update({"a"}, 2 * kSec).empty());
	CHECK(tracker.update({"a", "b"}, 3 * kSec) == std::vector<std::string>{"b"});
	CHECK(tracker.update({"a", "b"}, 4 * kSec).empty());
	// 空の userId は数えない。
	CHECK(tracker.update({"a", "b", ""}, 5 * kSec).empty());
}

TEST_CASE("共有をやめて、時間を空けて始め直すと、新しい共有の始まりになる")
{
	ScreenShareTracker tracker;
	tracker.update({"a"}, 1 * kSec);
	tracker.update({}, 2 * kSec); // やめた
	// 猶予時間の中に戻れば、同じ共有の続き (Discord の側で流れが切れて戻った)。
	CHECK(tracker.update({"a"}, 2 * kSec + kShareResumeWindowUs).empty());

	// 猶予時間を過ぎてから戻れば、新しい共有。
	tracker.update({}, 100 * kSec);
	CHECK(tracker.update({"a"}, 100 * kSec + kShareResumeWindowUs + 1) == std::vector<std::string>{"a"});
}

TEST_CASE("送信側との接続が切れて猶予時間の中につながり直した共有は続きとして扱い、猶予時間を過ぎたら新しい共有にする")
{
	ScreenShareTracker tracker;
	tracker.update({"a", "b"}, 1 * kSec);

	// 切れて、2 秒後につながり直した (送信側の再接続の間隔の範囲)。最初の configure に同じ人がいる。
	tracker.on_link_lost(10 * kSec);
	CHECK(tracker.update({"a", "b"}, 12 * kSec).empty());

	// もう一度切れ、猶予時間を過ぎてから戻った。
	tracker.on_link_lost(20 * kSec);
	CHECK(tracker.update({"a"}, 20 * kSec + kShareResumeWindowUs + kSec) == std::vector<std::string>{"a"});
	// b は戻らなかったので、後で戻れば新しい共有。
	CHECK(tracker.update({"a", "b"}, 60 * kSec) == std::vector<std::string>{"b"});

	// 切れている間に共有していなかった人は、猶予時間の中でも新しい共有。
	tracker.on_link_lost(100 * kSec);
	CHECK(tracker.update({"a", "c"}, 101 * kSec) == std::vector<std::string>{"c"});
}

TEST_CASE("共有の始まりで画面の音をミュートにしても、共有の途中で外したミュートは、続きの間は外れたまま")
{
	MixerLevels levels;
	ScreenShareTracker tracker;

	// pipeline.cpp の note_screen_shares_locked と同じ手順。
	const auto configure = [&](const std::set<std::string> &sharing, std::int64_t now_us) {
		for (const std::string &user_id : tracker.update(sharing, now_us))
			levels.mute_for_share_start(user_id);
	};

	levels.set_volume_pct(screen_key("a"), 60); // 覚えていた音量
	configure({"a"}, 1 * kSec);
	CHECK(levels.get(screen_key("a")).muted);
	CHECK(levels.get(screen_key("a")).volume_pct == 60);

	levels.set_mute(screen_key("a"), false); // 利用者が外した
	configure({"a"}, 2 * kSec); // 枠の付け替えなどで configure が来ても、共有は続いている
	CHECK_FALSE(levels.get(screen_key("a")).muted);

	// 接続が切れてすぐにつながり直しても、外したまま。
	tracker.on_link_lost(3 * kSec);
	configure({"a"}, 4 * kSec);
	CHECK_FALSE(levels.get(screen_key("a")).muted);

	// 共有をやめて始め直す (猶予時間を過ぎる) と、またミュートから始まる。音量は覚えた値のまま。
	configure({}, 5 * kSec);
	configure({"a"}, 5 * kSec + kShareResumeWindowUs + kSec);
	CHECK(levels.get(screen_key("a")).muted);
	CHECK(levels.get(screen_key("a")).volume_pct == 60);
}

TEST_CASE("画面共有している人は、configure の画面のストリームの人と、facts の sharing の人の和 (sharing が信じられないときは configure だけ)")
{
	CHECK(screen_sharers_of({"a"}, true, {"b"}) == std::set<std::string>{"a", "b"});
	CHECK(screen_sharers_of({"a"}, true, {"a"}) == std::set<std::string>{"a"});
	CHECK(screen_sharers_of({}, true, {"b"}) == std::set<std::string>{"b"});
	CHECK(screen_sharers_of({"a"}, false, {"b"}) == std::set<std::string>{"a"});
	CHECK(screen_sharers_of({}, false, {}).empty());
}

// 枠に載せた人だけ (視聴を始めるまでストリームが現れない) と、configure と facts の届く順を、pipeline.cpp の
// note_screen_shares_locked と同じ手順で流す。configure に現れる人は視聴の範囲の人だけ、facts の sharing は共有している間
// ずっと載っている。
struct ShareFlow {
	MixerLevels levels;
	ScreenShareTracker tracker;
	std::set<std::string> in_configure;
	bool sharing_valid = false;
	std::vector<std::string> in_facts;
	int mutes = 0; // 始まりとして数えた回数

	void note(std::int64_t now_us)
	{
		for (const std::string &user_id : tracker.update(screen_sharers_of(in_configure, sharing_valid, in_facts), now_us)) {
			levels.mute_for_share_start(user_id);
			++mutes;
		}
	}
};

TEST_CASE("共有を始めると 1 回だけミュートにし、視聴を始めてストリームが現れても、もう一度ミュートにしない")
{
	ShareFlow f;
	// facts が先に届く (共有している。ストリームはまだない)。
	f.sharing_valid = true;
	f.in_facts = {"a"};
	f.note(1 * kSec);
	CHECK(f.mutes == 1);
	CHECK(f.levels.get(screen_key("a")).muted);

	// 載せる前に、スピーカーのボタンでミュートを外す。
	f.levels.set_mute(screen_key("a"), false);
	// チェックを付けて視聴が始まり、configure にストリームが現れる。始まりではない。
	f.in_configure = {"a"};
	f.note(2 * kSec);
	CHECK(f.mutes == 1);
	CHECK_FALSE(f.levels.get(screen_key("a")).muted);
}

TEST_CASE("configure が先でも facts が先でも、共有の始まりは 1 回")
{
	ShareFlow configure_first;
	configure_first.in_configure = {"a"};
	configure_first.note(1 * kSec);
	configure_first.sharing_valid = true;
	configure_first.in_facts = {"a"};
	configure_first.note(2 * kSec);
	CHECK(configure_first.mutes == 1);

	ShareFlow facts_first;
	facts_first.sharing_valid = true;
	facts_first.in_facts = {"a"};
	facts_first.note(1 * kSec);
	facts_first.in_configure = {"a"};
	facts_first.note(2 * kSec);
	CHECK(facts_first.mutes == 1);
}

TEST_CASE("チェックを外して 10 秒以上たってから付け直しても、共有が続いていればミュートに戻さない")
{
	ShareFlow f;
	f.sharing_valid = true;
	f.in_facts = {"a"};
	f.in_configure = {"a"};
	f.note(1 * kSec);
	f.levels.set_mute(screen_key("a"), false);

	// チェックを外す (視聴をやめるのでストリームが消える。facts の sharing には残る)。
	f.in_configure = {};
	f.note(2 * kSec);
	f.note(2 * kSec + kShareResumeWindowUs + 5 * kSec);
	// 付け直す。
	f.in_configure = {"a"};
	f.note(2 * kSec + kShareResumeWindowUs + 6 * kSec);
	CHECK(f.mutes == 1);
	CHECK_FALSE(f.levels.get(screen_key("a")).muted);
}

TEST_CASE("共有をやめて猶予時間を過ぎてから始め直すと、ミュートに戻す (facts の sharing からも消えたとき)")
{
	ShareFlow f;
	f.sharing_valid = true;
	f.in_facts = {"a"};
	f.in_configure = {"a"};
	f.note(1 * kSec);
	f.levels.set_mute(screen_key("a"), false);

	f.in_facts = {};
	f.in_configure = {};
	f.note(2 * kSec);
	f.in_facts = {"a"};
	f.note(2 * kSec + kShareResumeWindowUs + kSec);
	CHECK(f.mutes == 2);
	CHECK(f.levels.get(screen_key("a")).muted);

	// 猶予時間の中に始め直したら、続きとして扱う。
	f.levels.set_mute(screen_key("a"), false);
	f.in_facts = {};
	f.note(30 * kSec);
	f.in_facts = {"a"};
	f.note(31 * kSec);
	CHECK(f.mutes == 2);
	CHECK_FALSE(f.levels.get(screen_key("a")).muted);
}

TEST_CASE("facts の sharing が信じられない間は configure の人だけで見分ける (全員を受け取る設定の動きと同じ)")
{
	ShareFlow f;
	f.sharing_valid = false;
	f.in_facts = {"a"}; // 値は信じない
	f.note(1 * kSec);
	CHECK(f.mutes == 0);
	f.in_configure = {"a"};
	f.note(2 * kSec);
	CHECK(f.mutes == 1);
	CHECK(f.levels.get(screen_key("a")).muted);
	// 接続が切れても、猶予時間の中につながり直せば続き。
	f.tracker.on_link_lost(3 * kSec);
	f.levels.set_mute(screen_key("a"), false);
	f.note(5 * kSec);
	CHECK(f.mutes == 1);
}

TEST_CASE("参加者ごとの音量とミュートは、その人の組が名簿から外れて戻っても表に残り、戻るたびに組へ当たり直る")
{
	cordmux::pipeline::AudioMixerConfig config;
	config.lookahead_us = 20000;
	cordmux::pipeline::AudioMixer mixer(config);
	cordmux::pipeline::MixerRoster roster;
	MixerLevels levels;
	levels.set_volume_pct(voice_key("a"), 50);
	levels.set_mute(voice_key("a"), true);

	// pipeline.cpp の apply_mixer_changes_locked と同じ手順。外れた組は AudioMixer から消し、入った組には表の値を当てる。
	const auto apply = [&](const cordmux::pipeline::MixerRoster::Change &change) {
		for (const MixerKey &key : change.removed)
			mixer.remove(key);
		for (const MixerKey &key : change.added)
			apply_mixer_level(mixer, levels, key);
	};
	const std::map<std::uint32_t, MixerKey> present{{1, voice_key("a")}};

	constexpr std::int64_t kT = 1000000;
	std::vector<float> chunk(480 * 2, 0.4f);
	const auto all_silent = [](const std::vector<cordmux::pipeline::MixedBlock> &blocks) {
		for (const cordmux::pipeline::MixedBlock &block : blocks) {
			for (const float sample : block.samples) {
				if (sample != 0.0f)
					return false;
			}
		}
		return true;
	};

	apply(roster.sync(present));
	mixer.add(kT, voice_key("a"), kT, chunk.data(), 480);
	CHECK(all_silent(mixer.take(kT)));

	// 通話を抜ける、Vesktop が繋ぎ直す、一覧が空になる、ダミーに入って出る、のどれでも組は名簿から外れる。
	apply(roster.sync({}));
	CHECK_FALSE(roster.contains(voice_key("a")));
	CHECK(levels.get(voice_key("a")).muted);
	CHECK(levels.get(voice_key("a")).volume_pct == 50);

	// 戻ると、ミキサーの組は利得 1・ミュートなしの新しい組になるので、表の値を当て直す。当て直した後も鳴らない。
	apply(roster.sync(present));
	CHECK(levels.get(voice_key("a")).muted);
	CHECK(levels.get(voice_key("a")).volume_pct == 50);
	constexpr std::int64_t kT2 = kT + 500000;
	mixer.add(kT2, voice_key("a"), kT2, chunk.data(), 480);
	CHECK(all_silent(mixer.take(kT2)));

	// 利用者が戻した後のミュートの解除は、音量 50% のまま鳴る。
	levels.set_mute(voice_key("a"), false);
	apply_mixer_level(mixer, levels, voice_key("a"));
	constexpr std::int64_t kT3 = kT2 + 500000;
	mixer.add(kT3, voice_key("a"), kT3, chunk.data(), 480);
	bool sounded = false;
	for (const cordmux::pipeline::MixedBlock &block : mixer.take(kT3)) {
		if (block.samples[0] != 0.0f) {
			CHECK(std::fabs(block.samples[0]) <= 0.2f + 1e-4f);
			sounded = true;
		}
	}
	CHECK(sounded);
}
