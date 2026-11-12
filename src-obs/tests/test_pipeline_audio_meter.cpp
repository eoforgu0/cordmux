// SPDX-License-Identifier: Apache-2.0

// pipeline/audio_meter.h と、AudioMixer::levels (参加者ごとの音の大きさ) の単体テスト。dBFS とメーターの位置への変換、
// すぐ上がりゆっくり下がる動き、利得とミュートを掛ける前と後の値、組の出入りと reset を確認する。

#include "doctest.h"

#include "pipeline/audio_meter.h"
#include "pipeline/audio_mixer.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using cordmux::pipeline::AudioMixer;
using cordmux::pipeline::AudioMixerConfig;
using cordmux::pipeline::AudioTrackLevel;
using cordmux::pipeline::kMeterFallDbPerSecond;
using cordmux::pipeline::kMixerSampleRate;
using cordmux::pipeline::kSilenceDb;
using cordmux::pipeline::meter_decay_per_block;
using cordmux::pipeline::meter_position;
using cordmux::pipeline::MixerKey;
using cordmux::pipeline::MixerKind;
using cordmux::pipeline::PeakHold;
using cordmux::pipeline::peak_to_dbfs;

namespace {

constexpr std::int64_t kT = 1000000;
constexpr std::int64_t kBlockUs = 10000;

MixerKey voice(const char *user) { return MixerKey{user, MixerKind::Voice}; }
MixerKey screen(const char *user) { return MixerKey{user, MixerKind::ScreenAudio}; }

void add_chunk(AudioMixer &mixer, std::int64_t now_us, const MixerKey &key, std::int64_t at_us, float v)
{
	// 20 ms (2 区間) ぶん。
	std::vector<float> data(960 * 2, v);
	mixer.add(now_us, key, at_us, data.data(), 960);
}

// 先渡しの幅を 20 ms にして、最初の take で 2 区間を出す設定 (既定の値には頼らない)。
AudioMixerConfig test_config()
{
	AudioMixerConfig config;
	config.lookahead_us = 20000;
	return config;
}

const AudioTrackLevel *find(const std::vector<AudioTrackLevel> &levels, const MixerKey &key)
{
	for (const AudioTrackLevel &level : levels) {
		if (level.key == key)
			return &level;
	}
	return nullptr;
}

} // namespace

TEST_CASE("振れ幅は dBFS になり、0 以下は無音の値になる")
{
	CHECK(peak_to_dbfs(1.0f) == doctest::Approx(0.0f));
	CHECK(peak_to_dbfs(0.5f) == doctest::Approx(-6.0206f).epsilon(0.001));
	CHECK(peak_to_dbfs(0.0f) == kSilenceDb);
	CHECK(peak_to_dbfs(-1.0f) == kSilenceDb);
	CHECK(peak_to_dbfs(std::numeric_limits<float>::quiet_NaN()) == kSilenceDb);
}

TEST_CASE("メーターの位置は -48 dB から 0 dB を 0 から 1 にし、範囲の外は端に収める")
{
	CHECK(meter_position(-48.0f) == doctest::Approx(0.0f));
	CHECK(meter_position(-24.0f) == doctest::Approx(0.5f));
	CHECK(meter_position(0.0f) == doctest::Approx(1.0f));
	CHECK(meter_position(-100.0f) == 0.0f);
	CHECK(meter_position(6.0f) == 1.0f);
	CHECK(meter_position(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

TEST_CASE("ピークは大きい値にすぐ上がり、小さい値には一定の速さで下がる")
{
	const float decay = meter_decay_per_block(480, kMixerSampleRate);
	// 10 ms で kMeterFallDbPerSecond / 100 dB 下がる。
	CHECK(20.0f * std::log10(decay) == doctest::Approx(-kMeterFallDbPerSecond / 100.0f).epsilon(0.001));

	PeakHold hold;
	hold.update(0.8f, decay);
	CHECK(hold.value() == doctest::Approx(0.8f));
	hold.update(0.0f, decay);
	CHECK(hold.value() == doctest::Approx(0.8f * decay));
	// 小さい山では上がらない。大きい山にはすぐ上がる。
	hold.update(0.1f, decay);
	CHECK(hold.value() == doctest::Approx(0.8f * decay * decay));
	hold.update(0.9f, decay);
	CHECK(hold.value() == doctest::Approx(0.9f));
}

TEST_CASE("ピークは 1 秒の無音で kMeterFallDbPerSecond だけ下がる")
{
	const float decay = meter_decay_per_block(480, kMixerSampleRate);
	PeakHold hold;
	hold.update(1.0f, decay);
	for (int i = 0; i < 100; ++i)
		hold.update(0.0f, decay);
	CHECK(peak_to_dbfs(hold.value()) == doctest::Approx(-kMeterFallDbPerSecond).epsilon(0.01));
}

TEST_CASE("下がり続けたピークは 0 になり、負の値と NaN は 0 として扱う")
{
	const float decay = meter_decay_per_block(480, kMixerSampleRate);
	PeakHold hold;
	hold.update(0.5f, decay);
	for (int i = 0; i < 2000; ++i)
		hold.update(0.0f, decay);
	CHECK(hold.value() == 0.0f);

	hold.update(-0.5f, decay);
	CHECK(hold.value() == 0.0f);
	hold.update(std::numeric_limits<float>::quiet_NaN(), decay);
	CHECK(hold.value() == 0.0f);
}

TEST_CASE("区間の長さかサンプルレートが 0 以下なら、下がらない")
{
	CHECK(meter_decay_per_block(480, 0) == 1.0f);
	CHECK(meter_decay_per_block(0, kMixerSampleRate) == 1.0f);
	CHECK(meter_decay_per_block(-5, kMixerSampleRate) == 1.0f);
}

TEST_CASE("levels は、利得を掛ける前と後のピークを組ごとに返す")
{
	AudioMixer mixer(test_config());
	mixer.set_gain(voice("a"), 0.5f);
	add_chunk(mixer, kT, voice("a"), kT, 0.4f);
	add_chunk(mixer, kT, screen("a"), kT, 0.2f);
	mixer.take(kT);

	const std::vector<AudioTrackLevel> levels = mixer.levels();
	REQUIRE(levels.size() == 2);
	// 並びは user_id の昇順で、同じ人は声、画面の音の順。
	CHECK(levels[0].key == voice("a"));
	CHECK(levels[1].key == screen("a"));
	CHECK(levels[0].peak_before == doctest::Approx(0.4f));
	CHECK(levels[0].peak_after == doctest::Approx(0.2f));
	CHECK_FALSE(levels[0].silenced);
	CHECK(levels[1].peak_before == doctest::Approx(0.2f));
	CHECK(levels[1].peak_after == doctest::Approx(0.2f));
}

TEST_CASE("ミュートか利得 0 の組は、掛けた後が 0 のまま、掛ける前は動き、silenced になる")
{
	AudioMixer mixer(test_config());
	mixer.set_mute(voice("a"), true);
	mixer.set_gain(screen("a"), 0.0f);
	add_chunk(mixer, kT, voice("a"), kT, 0.5f);
	add_chunk(mixer, kT, screen("a"), kT, 0.25f);
	mixer.take(kT);

	const std::vector<AudioTrackLevel> levels = mixer.levels();
	const AudioTrackLevel *v = find(levels, voice("a"));
	const AudioTrackLevel *s = find(levels, screen("a"));
	REQUIRE(v != nullptr);
	REQUIRE(s != nullptr);
	CHECK(v->silenced);
	CHECK(v->peak_before == doctest::Approx(0.5f));
	CHECK(v->peak_after == 0.0f);
	CHECK(s->silenced);
	CHECK(s->peak_before == doctest::Approx(0.25f));
	CHECK(s->peak_after == 0.0f);
}

TEST_CASE("音が来なくなると、区間ごとに下がり、読んでも値は変わらない")
{
	AudioMixer mixer(test_config());
	add_chunk(mixer, kT, voice("a"), kT, 0.5f);
	mixer.take(kT);
	const float first = mixer.levels()[0].peak_after;
	CHECK(first == doctest::Approx(0.5f));
	// 読み直しても同じ。
	CHECK(mixer.levels()[0].peak_after == first);

	// 音のない区間が進むと下がる。
	std::int64_t now = kT;
	for (int i = 0; i < 30; ++i) {
		now += kBlockUs;
		mixer.take(now);
	}
	const float later = mixer.levels()[0].peak_after;
	CHECK(later < first);
	CHECK(later > 0.0f);
}

TEST_CASE("組を外すと levels から消え、reset は残した組の値を 0 に戻す")
{
	AudioMixer mixer(test_config());
	add_chunk(mixer, kT, voice("a"), kT, 0.5f);
	add_chunk(mixer, kT, voice("b"), kT, 0.5f);
	mixer.take(kT);
	REQUIRE(mixer.levels().size() == 2);

	mixer.remove(voice("a"));
	const std::vector<AudioTrackLevel> after_remove = mixer.levels();
	REQUIRE(after_remove.size() == 1);
	CHECK(after_remove[0].key == voice("b"));

	mixer.reset();
	const std::vector<AudioTrackLevel> after_reset = mixer.levels();
	REQUIRE(after_reset.size() == 1);
	CHECK(after_reset[0].peak_before == 0.0f);
	CHECK(after_reset[0].peak_after == 0.0f);
}

TEST_CASE("組がない間の levels は空")
{
	AudioMixer mixer(test_config());
	CHECK(mixer.levels().empty());
}
