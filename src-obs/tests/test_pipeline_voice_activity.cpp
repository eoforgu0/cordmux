// SPDX-License-Identifier: Apache-2.0

// pipeline/voice_activity.h の単体テスト。区間ごとの「聞こえる声」から、参加者ごとの話し始めと話し終わりの時刻
// (ナノ秒) を作る処理の、時刻、息継ぎの扱い、複数人の並び、reset を確認する。最後のテストは、AudioMixer から取り出した
// 区間をそのまま渡す形 (pipeline.cpp の mixer_loop と同じ順) で、声が途中で届かなくなったときに光が残らないことを
// 確認する。

#include "doctest.h"

#include "pipeline/audio_mixer.h"
#include "pipeline/voice_activity.h"

#include <cstdint>
#include <string>
#include <vector>

using cordmux::pipeline::AudioMixer;
using cordmux::pipeline::kMixerChannels;
using cordmux::pipeline::kMixerSampleRate;
using cordmux::pipeline::kVoiceActivityGapUs;
using cordmux::pipeline::MixedBlock;
using cordmux::pipeline::MixerKey;
using cordmux::pipeline::MixerKind;
using cordmux::pipeline::VoiceActivity;
using cordmux::pipeline::VoiceActivityUpdate;

namespace {

constexpr std::int64_t kBlockUs = 10000;
constexpr std::int64_t kBase = 5000000; // 最初の区間の開始の時刻 (マイクロ秒)

using Heard = std::vector<std::string>;

// 区間 index (kBase から 10 ms ずつ) を見て、更新を返す。
std::vector<VoiceActivityUpdate> step(VoiceActivity &activity, int index, const Heard &audible)
{
	return activity.observe(kBase + index * kBlockUs, kBlockUs, audible);
}

constexpr int gap_blocks()
{
	return static_cast<int>(kVoiceActivityGapUs / kBlockUs);
}

} // namespace

TEST_CASE("聞こえる声が現れた区間の開始の時刻が、話し始めの時刻になる")
{
	VoiceActivity activity;
	CHECK(step(activity, 0, {}).empty());
	const std::vector<VoiceActivityUpdate> updates = step(activity, 1, {"a"});
	REQUIRE(updates.size() == 1);
	CHECK(updates[0].user_id == "a");
	CHECK(updates[0].start_ns == static_cast<std::uint64_t>(kBase + kBlockUs) * 1000);
	CHECK(updates[0].stop_ns == 0);
	CHECK(activity.talking_count() == 1);
}

TEST_CASE("話している間は更新を返さない")
{
	VoiceActivity activity;
	step(activity, 0, {"a"});
	for (int i = 1; i < 20; ++i)
		CHECK(step(activity, i, {"a"}).empty());
}

TEST_CASE("声が止まっても息継ぎの長さの間は話し終わりにせず、過ぎたら最後に音のあった区間の終わりを話し終わりにする")
{
	VoiceActivity activity;
	step(activity, 0, {"a"});
	step(activity, 1, {"a"}); // 最後に音のあった区間の終わりは kBase + 20 ms
	const std::int64_t last_end = kBase + 2 * kBlockUs;
	// 区間 2 から、音のない区間が続く。息継ぎの長さの手前までは話している。
	for (int i = 2; i < 2 + gap_blocks(); ++i)
		CHECK(step(activity, i, {}).empty());
	const std::vector<VoiceActivityUpdate> updates = step(activity, 2 + gap_blocks(), {});
	REQUIRE(updates.size() == 1);
	CHECK(updates[0].user_id == "a");
	CHECK(updates[0].start_ns == 0);
	CHECK(updates[0].stop_ns == static_cast<std::uint64_t>(last_end) * 1000);
	CHECK(activity.talking_count() == 0);
}

TEST_CASE("息継ぎの長さより短い無音は、話し終わりと話し始めを作らない")
{
	VoiceActivity activity;
	step(activity, 0, {"a"});
	CHECK(step(activity, 1, {}).empty());
	CHECK(step(activity, 2, {}).empty());
	CHECK(step(activity, 3, {"a"}).empty()); // 続けて話している
	CHECK(activity.talking_count() == 1);
}

TEST_CASE("話し終わりの後にまた聞こえたら、新しい話し始めを返す")
{
	VoiceActivity activity;
	step(activity, 0, {"a"});
	const int later = 1 + gap_blocks();
	REQUIRE(step(activity, later, {}).size() == 1); // 話し終わり
	const std::vector<VoiceActivityUpdate> updates = step(activity, later + 5, {"a"});
	REQUIRE(updates.size() == 1);
	CHECK(updates[0].start_ns == static_cast<std::uint64_t>(kBase + (later + 5) * kBlockUs) * 1000);
	CHECK(updates[0].stop_ns == 0);
}

TEST_CASE("複数人の更新は、話し終わり、話し始めの順で、それぞれ user_id の昇順に返す")
{
	VoiceActivity activity;
	step(activity, 0, {"b", "d"});
	const int later = 1 + gap_blocks();
	// b と d は止まり、a と c が話し始める区間。
	const std::vector<VoiceActivityUpdate> updates = step(activity, later, {"c", "a"});
	REQUIRE(updates.size() == 4);
	CHECK(updates[0].user_id == "b");
	CHECK(updates[0].stop_ns != 0);
	CHECK(updates[1].user_id == "d");
	CHECK(updates[1].stop_ns != 0);
	CHECK(updates[2].user_id == "a");
	CHECK(updates[2].start_ns != 0);
	CHECK(updates[3].user_id == "c");
	CHECK(updates[3].start_ns != 0);
}

TEST_CASE("空の user_id は読み飛ばし、同じ user_id が重なっても 1 人として扱う")
{
	VoiceActivity activity;
	const std::vector<VoiceActivityUpdate> updates = step(activity, 0, {"", "a", "a"});
	REQUIRE(updates.size() == 1);
	CHECK(updates[0].user_id == "a");
	CHECK(activity.talking_count() == 1);
}

TEST_CASE("reset は話し終わりを返さずに状態を捨て、次に聞こえたら話し始めになる")
{
	VoiceActivity activity;
	step(activity, 0, {"a"});
	activity.reset();
	CHECK(activity.talking_count() == 0);
	const std::vector<VoiceActivityUpdate> updates = step(activity, 1, {"a"});
	REQUIRE(updates.size() == 1);
	CHECK(updates[0].stop_ns == 0);
	CHECK(updates[0].start_ns != 0);
}

TEST_CASE("時刻が 0 以下でも、話し始めの時刻は 0 にならない")
{
	VoiceActivity activity;
	const std::vector<VoiceActivityUpdate> started = activity.observe(0, kBlockUs, {"a"});
	REQUIRE(started.size() == 1);
	CHECK(started[0].start_ns != 0);
}

TEST_CASE("話している人の声が途中で届かなくなっても、取り出す区間は無音で続き、息継ぎの長さの後に話し終わりになる")
{
	// pipeline.cpp の mixer_loop と同じく、10 ms ごとに AudioMixer から区間を取り出し、区間ごとに observe に渡す。
	// 声は最初の 200 ms だけ届き、その後は 1 つも届かない (送信側が送らなくなった、または途切れた)。声を置く時刻は
	// 届いた時刻 + 65 ms (composite_audio_plan.h の J と同じ長さ)。
	AudioMixer mixer;
	VoiceActivity activity;
	const MixerKey alice{"alice", MixerKind::Voice};
	const std::vector<float> loud(480 * kMixerChannels, 0.5f);
	const std::int64_t t0 = 10000000;
	std::vector<VoiceActivityUpdate> updates;
	std::int64_t last_audible_end_us = 0;
	int silent_blocks_after = 0;
	for (std::int64_t i = 0; i < 50; ++i) {
		const std::int64_t now = t0 + i * kBlockUs;
		if (i < 20)
			mixer.add(now, alice, now + 65000, loud.data(), 480);
		for (const MixedBlock &block : mixer.take(now)) {
			const std::int64_t block_us = std::int64_t(block.frames) * 1000000 / kMixerSampleRate;
			if (!block.audible_voices.empty())
				last_audible_end_us = block.start_us + block_us;
			else if (last_audible_end_us != 0)
				++silent_blocks_after;
			for (const VoiceActivityUpdate &u : activity.observe(block.start_us, block_us, block.audible_voices))
				updates.push_back(u);
		}
	}
	// 声が届かなくなった後も、区間は無音で取り出し続ける (AudioMixer::take は音のない区間も返す)。
	CHECK(silent_blocks_after > gap_blocks());
	REQUIRE(updates.size() == 2);
	CHECK(updates[0].user_id == "alice");
	CHECK(updates[0].start_ns != 0);
	CHECK(updates[1].user_id == "alice");
	CHECK(updates[1].stop_ns == std::uint64_t(last_audible_end_us) * 1000);
	CHECK(activity.talking_count() == 0);
}

TEST_CASE("talking_users は話している人を昇順で返し、話し終わりと reset で消える")
{
	VoiceActivity activity;
	CHECK(activity.talking_users().empty());

	step(activity, 0, {"b", "a"});
	CHECK(activity.talking_users() == std::vector<std::string>{"a", "b"});

	// a だけが続ける。b は息継ぎの長さまでは残り、過ぎると消える。
	for (int i = 1; i <= gap_blocks() + 1; ++i)
		step(activity, i, {"a"});
	CHECK(activity.talking_users() == std::vector<std::string>{"a"});

	activity.reset();
	CHECK(activity.talking_users().empty());
}
