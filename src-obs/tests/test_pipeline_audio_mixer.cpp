// SPDX-License-Identifier: Apache-2.0

// pipeline/audio_mixer.h の単体テスト。参加者ごとの声と画面の音を、別々の利得で混ぜて時刻の途切れない 1 本にするコンポーネントの、
// 混ぜた値、切りそろえないこと、区間の開始の時刻、置き直しのしきい値と at_us への追従、捨てて数える場合、利得と
// ミュートの直線の移り、組の取り外し、取り出しの遅れ、reset を確認する。

#include "doctest.h"

#include "pipeline/audio_mixer.h"
#include "pipeline/composite_audio_plan.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using cordmux::pipeline::audio_mixer_retention_frames;
using cordmux::pipeline::AudioMixer;
using cordmux::pipeline::AudioMixerConfig;
using cordmux::pipeline::AudioMixerKeyStats;
using cordmux::pipeline::AudioMixerStats;
using cordmux::pipeline::kMixerLookaheadUs;
using cordmux::pipeline::kMixerSampleRate;
using cordmux::pipeline::kObsMixSpanUs;
using cordmux::pipeline::MixedBlock;
using cordmux::pipeline::MixerKey;
using cordmux::pipeline::MixerKind;

namespace {

constexpr std::int64_t kT = 1000000; // 起点にする今 (マイクロ秒)

MixerKey voice(const char *user) { return MixerKey{user, MixerKind::Voice}; }
MixerKey screen(const char *user) { return MixerKey{user, MixerKind::ScreenAudio}; }

// 左が v、右が -v の一定の値のチャンク (チャンネルの取り違えも見つけられるように、左右で符号を変える)。
std::vector<float> chunk(float v, std::size_t frames = 480)
{
	std::vector<float> out(frames * 2);
	for (std::size_t i = 0; i < frames; ++i) {
		out[i * 2] = v;
		out[i * 2 + 1] = -v;
	}
	return out;
}

void add_chunk(AudioMixer &mixer, std::int64_t now_us, const MixerKey &key, std::int64_t at_us, float v,
	       std::size_t frames = 480)
{
	const std::vector<float> data = chunk(v, frames);
	mixer.add(now_us, key, at_us, data.data(), frames);
}

// テストで区間の数を数えやすいように、先渡しの幅を 20 ms にした設定 (既定の値には頼らない)。
AudioMixerConfig test_config()
{
	AudioMixerConfig config;
	config.lookahead_us = 20000;
	return config;
}

// 取り出しの遅れの扱いを確認しないテスト用の設定 (遅れた区間を飛ばさない)。
AudioMixerConfig no_skip_config()
{
	AudioMixerConfig config = test_config();
	config.late_limit_us = 1000000000000;
	return config;
}

float left(const std::vector<MixedBlock> &blocks, std::size_t block, std::size_t frame)
{
	return blocks.at(block).samples.at(frame * 2);
}

float right(const std::vector<MixedBlock> &blocks, std::size_t block, std::size_t frame)
{
	return blocks.at(block).samples.at(frame * 2 + 1);
}

// 区間の全フレームが同じ値 (左が v、右が -v) か。
bool block_is(const std::vector<MixedBlock> &blocks, std::size_t block, float v)
{
	const MixedBlock &b = blocks.at(block);
	for (std::size_t i = 0; i < b.frames; ++i) {
		if (std::fabs(b.samples[i * 2] - v) > 1e-6f || std::fabs(b.samples[i * 2 + 1] + v) > 1e-6f)
			return false;
	}
	return true;
}

const AudioMixerKeyStats *find_stats(const AudioMixerStats &stats, const MixerKey &key)
{
	for (const AudioMixerKeyStats &s : stats.keys) {
		if (s.key == key)
			return &s;
	}
	return nullptr;
}

} // namespace

TEST_CASE("2 人の声と 1 人の画面の音を別の時刻に置くと、出した区間は利得を掛けた和になる")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = voice("a");
	const MixerKey b = voice("b");
	const MixerKey s = screen("a");
	mixer.set_gain(a, 0.5f);
	mixer.set_gain(b, 2.0f);
	// 声 a は 20 ms から 50 ms、声 b は 20 ms から 40 ms、a の画面の音は 30 ms から 40 ms に置く。
	for (int i = 0; i < 3; ++i)
		add_chunk(mixer, kT, a, kT + 20000 + i * 10000, 0.1f);
	for (int i = 0; i < 2; ++i)
		add_chunk(mixer, kT, b, kT + 20000 + i * 10000, 0.2f);
	add_chunk(mixer, kT, s, kT + 30000, 0.3f);

	const std::vector<MixedBlock> blocks = mixer.take(kT + 60000); // 区間の終わりが 80 ms 以内の 8 区間
	REQUIRE(blocks.size() == 8);
	CHECK(blocks[0].start_us == kT);
	CHECK(block_is(blocks, 0, 0.0f));
	CHECK(block_is(blocks, 1, 0.0f));
	CHECK(block_is(blocks, 2, 0.05f + 0.4f));
	CHECK(block_is(blocks, 3, 0.05f + 0.4f + 0.3f));
	CHECK(block_is(blocks, 4, 0.05f));
	CHECK(block_is(blocks, 5, 0.0f));
	CHECK(blocks[2].frames == 480);
	CHECK(blocks[2].samples.size() == 960);
}

TEST_CASE("和が 1 を超えても切りそろえない")
{
	AudioMixer mixer(no_skip_config());
	add_chunk(mixer, kT, voice("a"), kT + 10000, 0.8f);
	add_chunk(mixer, kT, voice("b"), kT + 10000, 0.8f);
	add_chunk(mixer, kT, screen("c"), kT + 10000, -2.0f);
	mixer.set_gain(voice("a"), 3.0f);
	const std::vector<MixedBlock> blocks = mixer.take(kT + 30000);
	REQUIRE(blocks.size() >= 2);
	// a は利得 3 に直線で移るので、ここでは利得が 1 のまま掛かる b と画面の音だけでなく、最初の区間の a も 1 を超える。
	CHECK(left(blocks, 1, 479) == doctest::Approx(0.8f * 3.0f + 0.8f - 2.0f));
	CHECK(left(blocks, 1, 479) > 1.0f);
	CHECK(right(blocks, 1, 479) == doctest::Approx(-(0.8f * 3.0f + 0.8f - 2.0f)));
}

TEST_CASE("区間の開始の時刻は途切れず、音のない区間も無音で出し続ける")
{
	AudioMixer mixer(test_config());
	CHECK_FALSE(mixer.origin_us().has_value());
	CHECK_FALSE(mixer.next_block_start_us().has_value());
	std::int64_t expected = kT;
	std::size_t total = 0;
	for (int step = 0; step < 50; ++step) {
		const std::vector<MixedBlock> blocks = mixer.take(kT + step * 10000);
		for (const MixedBlock &block : blocks) {
			CHECK(block.start_us == expected);
			CHECK(block.frames == 480);
			expected += 10000;
			++total;
		}
	}
	CHECK(mixer.origin_us().value() == kT);
	CHECK(total > 40);
	CHECK(mixer.next_block_start_us().value() == expected);
}

TEST_CASE("1 時間実行しても区間の開始の時刻に丸めの誤差が積もらない")
{
	// 441 フレームは 9187.5 マイクロ秒で、区間の開始の時刻が整数のマイクロ秒にならない。
	AudioMixerConfig config = no_skip_config();
	config.block_frames = 441;
	AudioMixer mixer(config);
	auto expected_start = [](std::int64_t k) { return kT + (k * 441 * 1000000 + 24000) / 48000; };

	std::int64_t k = 0;
	for (int second = 0; second <= 3600; ++second) {
		const std::int64_t now = kT + std::int64_t(second) * 1000000;
		for (const MixedBlock &block : mixer.take(now)) {
			REQUIRE(block.start_us == expected_start(k));
			// 厳密な 9187.5 x k マイクロ秒との差は、どれだけ回っても 0.5 マイクロ秒以内である。
			REQUIRE(std::llabs((block.start_us - kT) * 2 - k * 18375) <= 1);
			++k;
		}
	}
	// 1 時間後の k 番目の区間の開始は、起点から 441 x k / 48000 秒の四捨五入にちょうど合う。
	CHECK(k > 390000);
	CHECK(mixer.next_block_start_us().value() == expected_start(k));
	CHECK(expected_start(k + 1) > kT + 3600 * 1000000LL + config.lookahead_us);
}

TEST_CASE("441 フレームのチャンクを続きに置いても、隙間も重なりもできない")
{
	AudioMixerConfig config = no_skip_config();
	config.block_frames = 441;
	AudioMixer mixer(config);
	auto at = [](std::int64_t k) { return kT + (k * 441 * 1000000 + 24000) / 48000; };
	// 声のチャンクの at_us は、四捨五入したマイクロ秒で渡す (毎回 0.5 マイクロ秒以内の誤差)。
	// 50 ms 先に置いて、置いた分を順に出していく。
	std::vector<MixedBlock> all;
	for (std::int64_t k = 0; k < 1000; ++k) {
		add_chunk(mixer, at(k), voice("a"), at(k) + 50000, 1.0f, 441);
		for (MixedBlock &block : mixer.take(at(k)))
			all.push_back(std::move(block));
	}
	for (MixedBlock &block : mixer.take(at(1000) + 200000))
		all.push_back(std::move(block));

	// 音は 2400 フレーム目から 443400 フレーム目まで続く。その間に完全に入る区間 6 から 1004 は、全部 1.0 である。
	REQUIRE(all.size() > 1010);
	for (std::size_t i = 6; i <= 1004; ++i)
		REQUIRE(block_is(all, i, 1.0f));
	CHECK(block_is(all, 5, 0.0f) == false);
	CHECK(block_is(all, 1006, 0.0f));
	const AudioMixerStats stats = mixer.take_stats();
	const AudioMixerKeyStats *key = find_stats(stats, voice("a"));
	REQUIRE(key != nullptr);
	CHECK(key->replaced == 0);
	CHECK(key->placed_frames == 441000);
	CHECK(key->late_frames == 0);
}

TEST_CASE("続きの位置と at_us の差が置き直しのしきい値の内なら続きに置き、超えたら at_us に置き直して数える")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = screen("a");
	add_chunk(mixer, kT, a, kT + 20000, 1.0f); // 20 ms から 30 ms
	// 続きの時刻は 30 ms。44.999 ms 後でも、画面の音の組のしきい値 (既定の 45 ms) の内なので続きに置く。
	add_chunk(mixer, kT, a, kT + 30000 + 44999, 2.0f);
	const AudioMixerStats inside = mixer.take_stats();
	REQUIRE(find_stats(inside, a) != nullptr);
	CHECK(find_stats(inside, a)->replaced == 0);
	CHECK(find_stats(inside, a)->max_error_us == doctest::Approx(44999.0).epsilon(0.001));
	// 続きの時刻 (40 ms 付近) より 46 ms 後は、しきい値の外なので at_us (86 ms) に置き直す。
	add_chunk(mixer, kT, a, kT + 86000, 3.0f);

	const std::vector<MixedBlock> blocks = mixer.take(kT + 200000);
	REQUIRE(blocks.size() >= 13);
	CHECK(block_is(blocks, 1, 0.0f));
	CHECK(block_is(blocks, 2, 1.0f));
	// 2 つ目のチャンクは 30 ms の続きに置く。速さを少し変えて書くので、境の数サンプルは前のチャンクの値と混ざる。
	CHECK(left(blocks, 3, 240) == doctest::Approx(2.0f).epsilon(1e-4));
	CHECK(block_is(blocks, 7, 0.0f));
	CHECK(left(blocks, 8, 400) == doctest::Approx(3.0f).epsilon(1e-4));
	CHECK(left(blocks, 9, 100) == doctest::Approx(3.0f).epsilon(1e-4));

	const AudioMixerStats stats = mixer.take_stats();
	const AudioMixerKeyStats *key = find_stats(stats, a);
	REQUIRE(key != nullptr);
	CHECK(key->replaced == 1);
}

TEST_CASE("続きの位置より前の at_us も、しきい値の内なら続きに置く")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = screen("a");
	add_chunk(mixer, kT, a, kT + 20000, 1.0f);
	add_chunk(mixer, kT, a, kT + 30000 - 44999, 2.0f); // 続きより 44.999 ms 前
	const std::vector<MixedBlock> blocks = mixer.take(kT + 100000);
	CHECK(block_is(blocks, 2, 1.0f));
	CHECK(left(blocks, 3, 240) == doctest::Approx(2.0f).epsilon(1e-4));
	CHECK(mixer.take_stats().keys.front().replaced == 0);
}

TEST_CASE("組ごとに、音の時計と OBS の時計の速さの差を推定し、置く位置を at_us に追従させる")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = screen("a");
	// 音の時計が OBS の時計より 300 ppm 遅い: 480 サンプルのチャンクは、OBS の時計で 10 ms x (1 + 300e-6) ごとに届く。
	const double period_us = 10000.0 * (1.0 + 300e-6);
	std::int64_t now = kT;
	for (int i = 0; i < 12000; ++i) {
		const std::int64_t at = kT + 50000 + std::int64_t(std::llround(double(i) * period_us));
		now = at - 50000;
		add_chunk(mixer, now, a, at, 0.5f);
		mixer.take(now);
	}
	const AudioMixerStats stats = mixer.take_stats();
	const AudioMixerKeyStats *key = find_stats(stats, a);
	REQUIRE(key != nullptr);
	CHECK(key->replaced == 0);
	CHECK(key->late_frames == 0);
	CHECK(key->drift_ppm == doctest::Approx(300.0).epsilon(0.05));
	// 2 分たった後も、置く位置と at_us の差は 1 サンプル未満に収まっている。
	add_chunk(mixer, now, a, kT + 50000 + std::int64_t(std::llround(12000.0 * period_us)), 0.5f);
	const AudioMixerStats last = mixer.take_stats();
	REQUIRE(find_stats(last, a) != nullptr);
	CHECK(find_stats(last, a)->max_error_us < 21.0);
}

TEST_CASE("resync の後の add は at_us に置き直し、前に戻ったときはそこから後ろを上書きする")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = voice("a");
	add_chunk(mixer, kT, a, kT + 20000, 1.0f);
	add_chunk(mixer, kT, a, kT + 30000, 2.0f);
	add_chunk(mixer, kT, a, kT + 40000, 3.0f);
	mixer.resync(a);
	// 30 ms に置き直す。許容幅の内の差 (続きは 50 ms) でも resync の後なので、必ず at_us に置く。
	add_chunk(mixer, kT, a, kT + 30000, 9.0f);

	const std::vector<MixedBlock> blocks = mixer.take(kT + 100000);
	CHECK(block_is(blocks, 2, 1.0f));
	CHECK(block_is(blocks, 3, 9.0f));
	CHECK(block_is(blocks, 4, 0.0f)); // 前の値 3.0 は上書きされて残らない
	CHECK(block_is(blocks, 5, 0.0f));
	// resync の置き直しは数えない。
	CHECK(mixer.take_stats().keys.front().replaced == 0);
}

TEST_CASE("resync の後の add は、続きより先の at_us にも置く")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = screen("a");
	add_chunk(mixer, kT, a, kT + 20000, 1.0f);
	mixer.resync(a);
	add_chunk(mixer, kT, a, kT + 35000, 2.0f); // 続き (30 ms) の許容幅の内。D を縮めた後を想定
	const std::vector<MixedBlock> blocks = mixer.take(kT + 100000);
	CHECK(block_is(blocks, 2, 1.0f));
	// 35 ms から 45 ms。区間 3 の後ろ半分と区間 4 の前半分。
	CHECK(left(blocks, 3, 100) == doctest::Approx(0.0f));
	CHECK(left(blocks, 3, 300) == doctest::Approx(2.0f));
	CHECK(left(blocks, 4, 100) == doctest::Approx(2.0f));
	CHECK(left(blocks, 4, 300) == doctest::Approx(0.0f));
}

TEST_CASE("遅れて届いた分と先すぎる分を捨てて数える")
{
	AudioMixerConfig config = no_skip_config();
	config.max_ahead_frames = 4800; // 100 ms
	AudioMixer mixer(config);
	const MixerKey a = voice("a");
	add_chunk(mixer, kT, a, kT + 5000, 1.0f); // 起点を決める
	const std::vector<MixedBlock> first = mixer.take(kT + 100000); // 120 ms (5760 フレーム) まで出し終える
	REQUIRE(first.size() == 12);

	// すべて出し終えた位置より前 (100 ms から 110 ms): 全部捨てる。
	mixer.resync(a);
	add_chunk(mixer, kT, a, kT + 100000, 5.0f);
	// 115 ms から 125 ms: 120 ms より前の 240 フレームを捨て、残りの 240 フレームを置く。
	mixer.resync(a);
	add_chunk(mixer, kT, a, kT + 115000, 6.0f);
	// 出し終えた位置 + 上限 (100 ms) を超える 218.75 ms から 228.75 ms: 全部捨てる。
	mixer.resync(a);
	add_chunk(mixer, kT, a, kT + 218750, 7.0f);
	// 上限にちょうど収まる 210 ms から 220 ms: 置く。
	mixer.resync(a);
	add_chunk(mixer, kT, a, kT + 210000, 8.0f);

	AudioMixerStats stats = mixer.take_stats();
	const AudioMixerKeyStats *key = find_stats(stats, a);
	REQUIRE(key != nullptr);
	CHECK(key->late_frames == 480 + 240);
	CHECK(key->ahead_frames == 480);
	CHECK(key->placed_frames == 480 + 240 + 480);

	const std::vector<MixedBlock> blocks = mixer.take(kT + 250000);
	// 区間 12 は 120 ms から 130 ms。置いたのは 120 ms から 125 ms の前半だけ。
	CHECK(left(blocks, 0, 100) == doctest::Approx(6.0f));
	CHECK(left(blocks, 0, 300) == doctest::Approx(0.0f));
	// 210 ms は区間 21。
	CHECK(block_is(blocks, 9, 8.0f));
}

TEST_CASE("出し終えた位置より前に置き直すと、そこから後ろの古い音も消える")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = voice("a");
	add_chunk(mixer, kT, a, kT + 20000, 1.0f);
	add_chunk(mixer, kT, a, kT + 30000, 2.0f);
	CHECK(mixer.take(kT + 20000).size() == 4); // 区間 0 から 3 まで (40 ms)。区間 3 の 2.0 は出し終える
	mixer.resync(a);
	add_chunk(mixer, kT, a, kT + 5000, 4.0f); // 全部が出し終えた位置より前
	CHECK(mixer.take_stats().keys.front().late_frames == 480);
	const std::vector<MixedBlock> rest = mixer.take(kT + 100000);
	CHECK(block_is(rest, 0, 0.0f));
}

TEST_CASE("利得の変更は次の区間から直線で反映され、その後は一定になる")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = voice("a");
	for (int i = 0; i < 10; ++i)
		add_chunk(mixer, kT, a, kT + 20000 + i * 10000, 1.0f);
	const std::vector<MixedBlock> before = mixer.take(kT + 30000); // 区間 0 から 4
	REQUIRE(before.size() == 5);
	CHECK(block_is(before, 2, 1.0f));
	CHECK(block_is(before, 4, 1.0f));

	mixer.set_gain(a, 0.0f);
	const std::vector<MixedBlock> after = mixer.take(kT + 60000); // 区間 5 から 7
	REQUIRE(after.size() == 3);
	for (std::size_t i = 0; i < 480; ++i) {
		const float expected = 1.0f - static_cast<float>(i + 1) / 480.0f;
		REQUIRE(left(after, 0, i) == doctest::Approx(expected).epsilon(1e-4));
		REQUIRE(right(after, 0, i) == doctest::Approx(-expected).epsilon(1e-4));
	}
	CHECK(block_is(after, 1, 0.0f));
	CHECK(block_is(after, 2, 0.0f));
}

TEST_CASE("ミュートと解除は利得 0 への直線の移りと、元の利得への直線の戻りになる")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = voice("a");
	mixer.set_gain(a, 0.5f);
	for (int i = 0; i < 20; ++i)
		add_chunk(mixer, kT, a, kT + 20000 + i * 10000, 1.0f);
	CHECK(block_is(mixer.take(kT + 30000), 4, 0.5f));

	mixer.set_mute(a, true);
	const std::vector<MixedBlock> muted = mixer.take(kT + 50000); // 区間 5 から 6
	REQUIRE(muted.size() == 2);
	CHECK(left(muted, 0, 0) == doctest::Approx(0.5f * (1.0f - 1.0f / 480.0f)).epsilon(1e-4));
	CHECK(std::fabs(left(muted, 0, 479)) < 1e-6f);
	CHECK(block_is(muted, 1, 0.0f));

	mixer.set_mute(a, false);
	const std::vector<MixedBlock> back = mixer.take(kT + 70000); // 区間 7 から 8
	REQUIRE(back.size() == 2);
	CHECK(left(back, 0, 0) == doctest::Approx(0.5f * 1.0f / 480.0f).epsilon(1e-4));
	CHECK(left(back, 0, 479) == doctest::Approx(0.5f).epsilon(1e-4));
	CHECK(block_is(back, 1, 0.5f));
}

TEST_CASE("利得は出すときに掛かるので、先に置いた画面の音にも変更がすぐ動作する")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey s = screen("a");
	add_chunk(mixer, kT, s, kT + 500000, 1.0f); // D = 500 ms 先に置く
	mixer.take(kT + 100000);
	mixer.set_gain(s, 0.25f); // 置いた後に変える
	const std::vector<MixedBlock> blocks = mixer.take(kT + 600000);
	// 区間 50 は 500 ms から 510 ms。置いたときの利得ではなく、出すときの利得が掛かる。
	REQUIRE(blocks.size() > 40);
	std::size_t index = 0;
	while (index < blocks.size() && blocks[index].start_us != kT + 500000)
		++index;
	REQUIRE(index < blocks.size());
	CHECK(block_is(blocks, index, 0.25f));
}

TEST_CASE("remove した組は次の区間で 0 へ移ってから消え、同じ組を足すと新しい組になる")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = voice("a");
	for (int i = 0; i < 10; ++i)
		add_chunk(mixer, kT, a, kT + 20000 + i * 10000, 1.0f);
	CHECK(mixer.take(kT + 30000).size() == 5);
	CHECK(mixer.track_count() == 1);

	mixer.remove(a);
	CHECK(mixer.track_count() == 0);
	const std::vector<MixedBlock> blocks = mixer.take(kT + 60000); // 区間 5 から 7
	REQUIRE(blocks.size() == 3);
	CHECK(left(blocks, 0, 0) == doctest::Approx(1.0f - 1.0f / 480.0f).epsilon(1e-4));
	CHECK(std::fabs(left(blocks, 0, 479)) < 1e-6f);
	CHECK(block_is(blocks, 1, 0.0f)); // 置いてあった音 (区間 6 から 11) は残らない
	CHECK(block_is(blocks, 2, 0.0f));

	// 外した後に同じ組へ足すと、前の利得を引き継がない (利得 1)。
	add_chunk(mixer, kT + 60000, a, kT + 100000, 0.5f);
	const std::vector<MixedBlock> again = mixer.take(kT + 150000);
	std::size_t index = 0;
	while (again[index].start_us != kT + 100000)
		++index;
	CHECK(block_is(again, index, 0.5f));
}

TEST_CASE("音のない組を remove すると、すぐ消える")
{
	AudioMixer mixer(test_config());
	mixer.set_gain(voice("a"), 0.3f);
	mixer.set_mute(screen("a"), true);
	CHECK(mixer.track_count() == 2);
	mixer.remove(voice("a"));
	mixer.remove(screen("a"));
	mixer.remove(voice("nobody")); // ない組を外しても何も起きない
	CHECK(mixer.track_count() == 0);
}

TEST_CASE("取り出しの遅れで区間がたまったら続けて出し、数える")
{
	AudioMixer mixer(test_config()); // 遅れの限度 20 ms
	CHECK(mixer.take(kT).size() == 2); // 起点で 0 ms と 10 ms の区間
	// 35 ms 時点の取り出し。区間 2 から 4 (20 ms、30 ms、40 ms) を出す。20 ms と 30 ms は now (35 ms) より前。
	const std::vector<MixedBlock> blocks = mixer.take(kT + 35000);
	REQUIRE(blocks.size() == 3);
	CHECK(blocks[0].start_us == kT + 20000);
	CHECK(blocks[1].start_us == kT + 30000);
	CHECK(blocks[2].start_us == kT + 40000);
	const AudioMixerStats stats = mixer.take_stats();
	CHECK(stats.blocks == 5);
	CHECK(stats.catch_up_blocks == 2);
	CHECK(stats.skips == 0);
	CHECK(stats.skipped_frames == 0);
}

TEST_CASE("遅れが限度を超えたら、古い区間は出さずに出す位置を今へ進め、飛ばした長さを数える")
{
	AudioMixer mixer(test_config());
	CHECK(mixer.take(kT).size() == 2);
	// 65 ms 時点。限度 20 ms より古い 45 ms 未満の区間 (20 ms、30 ms、40 ms の 3 区間 = 1440 フレーム) を飛ばす。
	const std::vector<MixedBlock> blocks = mixer.take(kT + 65000);
	REQUIRE(blocks.size() == 3);
	CHECK(blocks[0].start_us == kT + 50000);
	CHECK(blocks[1].start_us == kT + 60000);
	CHECK(blocks[2].start_us == kT + 70000);
	const AudioMixerStats stats = mixer.take_stats();
	CHECK(stats.blocks == 5);
	CHECK(stats.skips == 1);
	CHECK(stats.skipped_frames == 1440);
	CHECK(stats.catch_up_blocks == 2);
	// 飛ばしても格子はずれない。
	CHECK(mixer.next_block_start_us().value() == kT + 80000);
}

TEST_CASE("大きく遅れても、出す区間の数は限度の内に収まる")
{
	AudioMixer mixer(test_config());
	CHECK(mixer.take(kT).size() == 2);
	const std::vector<MixedBlock> blocks = mixer.take(kT + 10000000); // 10 秒遅れ
	CHECK(blocks.size() <= 5);
	for (const MixedBlock &block : blocks)
		CHECK(block.start_us >= kT + 10000000 - 20000);
	CHECK(mixer.take_stats().skipped_frames > 400000);
}

TEST_CASE("最初の take で起点が決まり、add の前でも無音を出し続ける")
{
	AudioMixer mixer(no_skip_config());
	CHECK(mixer.take(kT).size() == 2);
	CHECK(mixer.origin_us().value() == kT);
	// 起点の後に届いた声は、起点より後ろの at_us に置ける。
	add_chunk(mixer, kT + 5000, voice("a"), kT + 40000, 1.0f);
	const std::vector<MixedBlock> blocks = mixer.take(kT + 100000);
	REQUIRE(!blocks.empty());
	CHECK(blocks[0].start_us == kT + 20000);
	CHECK(block_is(blocks, 2, 1.0f)); // 40 ms の区間
}

TEST_CASE("最初の add の now が起点になり、音の位置には依らない")
{
	AudioMixer mixer(no_skip_config());
	// 画面の音が 1 秒先に置かれても、起点は add の now。後から届く声が起点より前になることはない。
	add_chunk(mixer, kT, screen("a"), kT + 1000000, 1.0f);
	CHECK(mixer.origin_us().value() == kT);
	add_chunk(mixer, kT, voice("a"), kT + 30000, 2.0f);
	const std::vector<MixedBlock> blocks = mixer.take(kT + 60000);
	CHECK(block_is(blocks, 3, 2.0f));
	CHECK(mixer.take_stats().keys.size() == 2);
}

TEST_CASE("reset は起点と保持している音を忘れ、組と利得は残す")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = voice("a");
	mixer.set_gain(a, 0.5f);
	add_chunk(mixer, kT, a, kT + 20000, 1.0f);
	add_chunk(mixer, kT, a, kT + 500000, 1.0f);
	CHECK(mixer.take(kT + 30000).size() == 5);
	mixer.take_stats();

	mixer.reset();
	CHECK_FALSE(mixer.origin_us().has_value());
	CHECK_FALSE(mixer.next_block_start_us().has_value());
	CHECK(mixer.track_count() == 1);

	constexpr std::int64_t kT2 = 90000000;
	add_chunk(mixer, kT2, a, kT2 + 20000, 1.0f);
	CHECK(mixer.origin_us().value() == kT2);
	const std::vector<MixedBlock> blocks = mixer.take(kT2 + 600000);
	REQUIRE(blocks.size() > 55);
	CHECK(blocks[0].start_us == kT2);
	CHECK(block_is(blocks, 2, 0.5f)); // 利得 0.5 は残り、立ち上がりの直線もない
	CHECK(block_is(blocks, 3, 0.0f));
	CHECK(block_is(blocks, 50, 0.0f)); // reset の前に置いた 500 ms の音は残らない
	// reset の後の最初の add は、前のチャンクの続きとは見なさず、at_us に置く (置き直しとは数えない)。
	CHECK(mixer.take_stats().keys.front().replaced == 0);
}

TEST_CASE("統計は周期ごとに戻り、外した組の数も次の周期に含まれる")
{
	AudioMixer mixer(no_skip_config());
	add_chunk(mixer, kT, voice("a"), kT + 20000, 1.0f);
	add_chunk(mixer, kT, screen("b"), kT + 20000, 1.0f);
	mixer.remove(screen("b"));
	mixer.take(kT + 100000);

	AudioMixerStats stats = mixer.take_stats();
	CHECK(stats.keys.size() == 2);
	const AudioMixerKeyStats *removed = find_stats(stats, screen("b"));
	REQUIRE(removed != nullptr);
	CHECK(removed->placed_frames == 480);
	CHECK(stats.blocks == 12);

	stats = mixer.take_stats();
	CHECK(stats.keys.empty());
	CHECK(stats.blocks == 0);
}

TEST_CASE("保持するフレーム数は、遅れの上限と揺れと先渡しの幅から計算する")
{
	// (1000 + 100 + 20) ms = 53760 フレームに、チャンク 480 フレームを足す。
	CHECK(audio_mixer_retention_frames(1000000, 100000, 20000, 480) == 53760 + 480);
	// 負の引数は 0 として扱う。
	CHECK(audio_mixer_retention_frames(-1, -1, -1, -1) == 0);
	// 既定の設定は、声の遅延の上限 2000 ms、J 200 ms、先渡し kMixerLookaheadUs (55 ms) から計算した値になる。
	CHECK(AudioMixerConfig{}.max_ahead_frames == (2255 * 48) + 480);
	CHECK(AudioMixerConfig{}.lookahead_us == kMixerLookaheadUs);
}

TEST_CASE("不正な設定は使える値に修正する")
{
	AudioMixerConfig config;
	config.block_frames = 0;
	config.lookahead_us = -5;
	config.max_ahead_frames = 0;
	AudioMixer mixer(config);
	CHECK(mixer.config().block_frames == 1);
	CHECK(mixer.config().lookahead_us == 0);
	CHECK(mixer.config().max_ahead_frames == 1);
}

TEST_CASE("先渡しの既定は、取り出しが 35 ms 遅れても先に渡した分が残り、起点の直後の 1 回分も満たす")
{
	CHECK(kObsMixSpanUs == 21334);
	CHECK(kMixerLookaheadUs == 55000);
	// (1) 定常: 丸め 10 ms + 取り出しの間隔 10 ms + 取り出しの遅れ 35 ms を引いても 0 以上。
	CHECK(kMixerLookaheadUs - 10000 - 10000 - 35000 >= 0);
	// (2) 起点の直後: 最初の take で丸めの後も OBS の 1 回分より先まで出す。
	CHECK(kMixerLookaheadUs - 10000 >= kObsMixSpanUs);
	CHECK(AudioMixerConfig{}.lookahead_us == kMixerLookaheadUs);
}

TEST_CASE("最初の take で、起点から OBS の 1 回分より先まで出す")
{
	AudioMixer mixer;
	const std::vector<MixedBlock> blocks = mixer.take(kT);
	REQUIRE_FALSE(blocks.empty());
	CHECK(blocks.front().start_us == kT);
	const std::int64_t end_us =
		blocks.back().start_us + std::int64_t(blocks.back().frames) * 1000000 / kMixerSampleRate;
	CHECK(end_us - kT >= kObsMixSpanUs);
}

TEST_CASE("先に渡してある長さの最小は、take のはじめに測り、起点の直後の最初の take は測らない")
{
	AudioMixer mixer(test_config());
	// 最初の take は起点を決めるだけで測らない。
	CHECK(mixer.take(kT).size() == 2);
	AudioMixerStats stats = mixer.take_stats();
	CHECK_FALSE(stats.min_ahead_us.has_value());
	CHECK(stats.short_ahead_takes == 0);

	// 出し終えた位置は kT + 20 ms (区間 2 つ)。10 ms 後の take のはじめは 20 - 10 = 10 ms 先にある。
	CHECK(mixer.take(kT + 10000).size() == 1);
	stats = mixer.take_stats();
	REQUIRE(stats.min_ahead_us.has_value());
	CHECK(*stats.min_ahead_us == 10000);
}

TEST_CASE("起点が add で決まったときも、その後の最初の take は測らない")
{
	AudioMixer mixer(test_config());
	add_chunk(mixer, kT, voice("a"), kT + 20000, 1.0f);
	mixer.take(kT + 5000);
	CHECK_FALSE(mixer.take_stats().min_ahead_us.has_value());
	mixer.take(kT + 15000); // 2 回目は測る
	CHECK(mixer.take_stats().min_ahead_us.has_value());
}

TEST_CASE("先に渡してある長さの最小と足りなかった回数は、周期で区切られる")
{
	AudioMixer mixer;
	mixer.take(kT);
	// 10 ms ごとに取り出すと、既定の先渡し 55 ms では、はじめの長さは 55 ms の丸めの範囲に収まる。
	for (int step = 1; step <= 20; ++step)
		mixer.take(kT + step * 10000);
	AudioMixerStats stats = mixer.take_stats();
	REQUIRE(stats.min_ahead_us.has_value());
	// 区間の終わりで出す丸めで最大 10 ms (480 フレーム) 減り、取り出しの間隔の 10 ms もあるので、
	// 先渡しの幅 - 20 ms 以上、先渡しの幅 - 10 ms 未満の範囲に入る。
	CHECK(*stats.min_ahead_us >= kMixerLookaheadUs - 20000);
	CHECK(*stats.min_ahead_us < kMixerLookaheadUs);
	CHECK(stats.short_ahead_takes == 0);

	// 周期が変わると、最小も回数もやり直しになる。測らなかった周期は値を持たない。
	stats = mixer.take_stats();
	CHECK_FALSE(stats.min_ahead_us.has_value());
	CHECK(stats.short_ahead_takes == 0);
	mixer.take(kT + 210000);
	stats = mixer.take_stats();
	REQUIRE(stats.min_ahead_us.has_value());
}

TEST_CASE("取り出しが遅れて先に渡した分を使い切ったら、足りなかった take として数える")
{
	AudioMixer mixer;
	mixer.take(kT);
	for (int step = 1; step <= 5; ++step)
		mixer.take(kT + step * 10000);
	CHECK(mixer.take_stats().short_ahead_takes == 0);

	// 出し終えた位置は kT + 100 ms。40 ms 空けて取り出すと、はじめの長さは 10 ms で、OBS の 1 回分より短いが 0 以上
	// なので数えない (OBS は区間の終わりを過ぎてから混ぜるので、0 以上あれば付け直さない)。
	mixer.take(kT + 90000);
	AudioMixerStats stats = mixer.take_stats();
	CHECK(stats.short_ahead_takes == 0);
	REQUIRE(stats.min_ahead_us.has_value());
	CHECK(*stats.min_ahead_us == 10000);

	// 出し終えた位置は kT + 140 ms。70 ms 空けて取り出すと、はじめの長さは -20 ms で数える。
	mixer.take(kT + 160000);
	// その次は通常の間隔に戻り、数えない。
	mixer.take(kT + 170000);
	stats = mixer.take_stats();
	CHECK(stats.short_ahead_takes == 1);
	REQUIRE(stats.min_ahead_us.has_value());
	CHECK(*stats.min_ahead_us == -20000);

	// 遅れを飛ばして進めた後も、はじめに測った値が残る (飛ばしで不足が隠れない)。
	AudioMixer late;
	late.take(kT);
	late.take(kT + 10000);
	late.take(kT + 10000000); // 10 秒遅れ
	const AudioMixerStats late_stats = late.take_stats();
	REQUIRE(late_stats.min_ahead_us.has_value());
	CHECK(*late_stats.min_ahead_us < 0);
	CHECK(late_stats.short_ahead_takes >= 1);
}

TEST_CASE("reset の後の最初の take は測らない")
{
	AudioMixer mixer;
	mixer.take(kT);
	mixer.take(kT + 10000);
	mixer.take_stats();
	mixer.reset();
	mixer.take(kT + 500000); // 起点を決め直すだけ
	CHECK_FALSE(mixer.take_stats().min_ahead_us.has_value());
}

// ---- 聞こえる音があった声の組 (MixedBlock::audible_voices) ----

TEST_CASE("声の組は、しきい値を超える音がある区間にだけ audible_voices に入り、無音の区間と画面の音の組は入らない")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = voice("a");
	const MixerKey b = voice("b");
	const MixerKey s = screen("c");
	// a は 10 ms から 20 ms の 1 区間、b は 0 から 20 ms の 2 区間、画面の音 c は 0 から 10 ms。
	add_chunk(mixer, kT, a, kT + 10000, 0.1f);
	add_chunk(mixer, kT, b, kT, 0.05f);
	add_chunk(mixer, kT, b, kT + 10000, 0.05f);
	add_chunk(mixer, kT, s, kT, 0.5f);

	const std::vector<MixedBlock> blocks = mixer.take(kT + 20000); // 区間の終わりが 40 ms 以内の 4 区間
	REQUIRE(blocks.size() == 4);
	// 区間 0 (0 から 10 ms): b の声だけ。画面の音は声ではない。
	CHECK(blocks[0].audible_voices == std::vector<std::string>{"b"});
	// 区間 1 (10 から 20 ms): a と b の声。並びは user_id の昇順。
	CHECK(blocks[1].audible_voices == std::vector<std::string>{"a", "b"});
	// 区間 2、3: 音がない。
	CHECK(blocks[2].audible_voices.empty());
	CHECK(blocks[3].audible_voices.empty());
}

TEST_CASE("聞こえる音の判断は利得を掛けた後の値で、しきい値ちょうどは聞こえるに入れない")
{
	AudioMixerConfig config = no_skip_config();
	config.audible_peak = 0.01f;
	AudioMixer mixer(config);
	const MixerKey quiet = voice("quiet");
	const MixerKey loud = voice("loud");
	const MixerKey edge = voice("edge");
	// 同じ 0.1 の音でも、利得 0.05 に下げた quiet は 0.005 で聞こえない。
	mixer.set_gain(quiet, 0.05f);
	add_chunk(mixer, kT, quiet, kT, 0.1f);
	add_chunk(mixer, kT, loud, kT, 0.1f);
	add_chunk(mixer, kT, edge, kT, 0.01f);

	const std::vector<MixedBlock> blocks = mixer.take(kT);
	REQUIRE_FALSE(blocks.empty());
	CHECK(blocks[0].audible_voices == std::vector<std::string>{"loud"});
}

TEST_CASE("既定のしきい値は -60 dBFS で、それより小さい音は聞こえるに入れない")
{
	CHECK(AudioMixerConfig{}.audible_peak == doctest::Approx(0.001f));
	AudioMixer mixer(no_skip_config());
	const MixerKey a = voice("a");
	const MixerKey b = voice("b");
	add_chunk(mixer, kT, a, kT, 0.0005f);
	add_chunk(mixer, kT, b, kT, 0.002f);
	const std::vector<MixedBlock> blocks = mixer.take(kT);
	REQUIRE_FALSE(blocks.empty());
	CHECK(blocks[0].audible_voices == std::vector<std::string>{"b"});
}

TEST_CASE("ミュートした声は、ミュートの直線の移りが終わった区間から聞こえるに入れない")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = voice("a");
	for (int i = 0; i < 6; ++i)
		add_chunk(mixer, kT, a, kT + i * 10000, 0.3f);

	std::vector<MixedBlock> blocks = mixer.take(kT); // 区間 0 (0 から 10 ms) と 1 を出す
	REQUIRE(blocks.size() >= 2);
	CHECK(blocks[0].audible_voices == std::vector<std::string>{"a"});
	mixer.set_mute(a, true);
	blocks = mixer.take(kT + 20000);
	REQUIRE(blocks.size() >= 2);
	// ミュートの直後の区間は、利得が 1 から 0 へ移る途中なので音がある。その次の区間からは無音になる。
	CHECK(blocks[0].audible_voices == std::vector<std::string>{"a"});
	CHECK(blocks[1].audible_voices.empty());
	CHECK(blocks.back().audible_voices.empty());
}

TEST_CASE("利得 0 の声は、音が置いてあっても聞こえるに入れない")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = voice("a");
	mixer.set_gain(a, 0.0f);
	add_chunk(mixer, kT, a, kT, 0.5f);
	const std::vector<MixedBlock> blocks = mixer.take(kT);
	REQUIRE_FALSE(blocks.empty());
	CHECK(blocks[0].audible_voices.empty());
}

TEST_CASE("remove して 0 へ移している最中の声は、聞こえるに入れない")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey a = voice("a");
	for (int i = 0; i < 4; ++i)
		add_chunk(mixer, kT, a, kT + i * 10000, 0.3f);
	mixer.take(kT);
	mixer.remove(a);
	const std::vector<MixedBlock> blocks = mixer.take(kT + 10000);
	REQUIRE_FALSE(blocks.empty());
	for (const MixedBlock &block : blocks)
		CHECK(block.audible_voices.empty());
}

TEST_CASE("利得とミュートを掛ける前の音で判定する組は、ミュートしていても聞こえるに入り、出す音には混ざらない")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey self = voice("self");
	const MixerKey other = voice("other");
	// 自分の声 (self) は、掛ける前の音で判定し、ミュートする。ほかの人 (other) は掛けた後の音で判定し、ミュートする。
	mixer.set_activity_before_gain(self, true);
	mixer.set_mute(self, true);
	mixer.set_mute(other, true);
	add_chunk(mixer, kT, self, kT, 0.3f);
	add_chunk(mixer, kT, other, kT, 0.3f);

	const std::vector<MixedBlock> blocks = mixer.take(kT);
	REQUIRE_FALSE(blocks.empty());
	CHECK(blocks[0].audible_voices == std::vector<std::string>{"self"});
	// 出す音には、どちらも混ざらない (音が置いてあるのは区間 0 だけ)。
	for (const float sample : blocks[0].samples)
		CHECK(sample == 0.0f);
}

TEST_CASE("利得とミュートを掛ける前の音で判定する組も、音のない区間としきい値以下の音は聞こえるに入れない")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey self = voice("self");
	mixer.set_activity_before_gain(self, true);
	mixer.set_gain(self, 0.0f);
	add_chunk(mixer, kT, self, kT, 0.0005f); // -60 dBFS より小さい
	add_chunk(mixer, kT, self, kT + 10000, 0.3f);

	const std::vector<MixedBlock> blocks = mixer.take(kT + 10000);
	REQUIRE(blocks.size() >= 3);
	CHECK(blocks[0].audible_voices.empty());
	CHECK(blocks[1].audible_voices == std::vector<std::string>{"self"});
	CHECK(blocks[2].audible_voices.empty());
}

TEST_CASE("remove した組は、掛ける前の音で判定する設定も既定 (掛けた後の音) に戻る")
{
	AudioMixer mixer(no_skip_config());
	const MixerKey self = voice("self");
	mixer.set_activity_before_gain(self, true);
	mixer.remove(self);
	mixer.set_mute(self, true);
	add_chunk(mixer, kT, self, kT, 0.3f);
	const std::vector<MixedBlock> blocks = mixer.take(kT);
	REQUIRE_FALSE(blocks.empty());
	CHECK(blocks[0].audible_voices.empty());
}

TEST_CASE("しきい値の負の値は 0 として扱い、0 のしきい値でも無音の組は聞こえるに入れない")
{
	AudioMixerConfig config = no_skip_config();
	config.audible_peak = -1.0f;
	AudioMixer mixer(config);
	CHECK(mixer.config().audible_peak == 0.0f);
	const MixerKey a = voice("a");
	const MixerKey zero = voice("zero");
	add_chunk(mixer, kT, a, kT, 0.0001f);
	add_chunk(mixer, kT, zero, kT, 0.0f);
	const std::vector<MixedBlock> blocks = mixer.take(kT);
	REQUIRE_FALSE(blocks.empty());
	CHECK(blocks[0].audible_voices == std::vector<std::string>{"a"});
}

// ---------------------------------------------------------------------------
// 製品の経路を通す長い模擬
// ---------------------------------------------------------------------------
//
// 実物の AudioMixer に、画面の音 (screen_audio_at_us で置く時刻を作る) と声 (voice_audio_at_us) のチャンクを、OBS の
// 取り出し (10 ms ごとの take) と交互に流す。音を取り出す時計 D は OBS の時計 Q より d_ppm 速く、10 時間の +-100 ppm
// (3.6 秒) と同じ桁の量 (500 ppm で 20 分 = 0.6 秒) を短い時間で起こす。音には 1 秒 (48000 サンプル) ごとに目印の
// パルス (画面の音は左、声は右のチャンネル) を入れ、take で出てきたサンプルの中のパルスの頂点の時刻を測る。画面の音の
// パルスが出るべき時刻は「そのサンプルを取り出した Q の時刻 + D + 半刻み」で、同じ中身の映像が出る時刻 (Q の時刻 + D。
// 刻みに合わせて平均で半刻み遅れる) と対になる。その差が時間で動かないことを確かめる。

namespace {

// 区間の中の目印のパルスの頂点の時刻 (us) を集める。パルスは 1 サンプルのインパルスで、変換で数サンプルの山になる。
class PulseFinder {
public:
	explicit PulseFinder(std::size_t channel) : channel_(channel) {}

	void feed(const MixedBlock &block)
	{
		for (std::size_t i = 0; i < block.frames; ++i) {
			const float v = block.samples[i * 2 + channel_];
			const double t = double(block.start_us) + double(i) * 1e6 / double(kMixerSampleRate);
			if (v > 0.2f) {
				if (!in_peak_ || v > peak_) {
					peak_ = v;
					peak_t_ = t;
				}
				in_peak_ = true;
			} else if (in_peak_) {
				pulses.push_back(peak_t_);
				in_peak_ = false;
				peak_ = 0.0f;
			}
		}
	}

	std::vector<double> pulses;

private:
	std::size_t channel_;
	bool in_peak_ = false;
	float peak_ = 0.0f;
	double peak_t_ = 0.0;
};

struct MixerRunResult {
	double screen_drift_us = 0.0;   // 画面の音: 最後のパルスのずれ - 60 秒の時点のずれ
	double screen_max_step_us = 0.0; // 画面の音: 隣り合うパルスのずれの差の最大
	double voice_drift_us = 0.0;
	double voice_max_step_us = 0.0;
	std::size_t screen_pulses = 0;
	std::size_t voice_pulses = 0;
	std::uint64_t screen_replaced = 0;
	std::uint64_t voice_replaced = 0;
	std::uint64_t late_frames = 0;
	std::uint64_t skips = 0;
	double screen_drift_ppm = 0.0;
};

MixerRunResult run_mixer_long(double d_ppm, double minutes, std::uint64_t seed)
{
	const double sample_q_us = 1e6 / (double(kMixerSampleRate) * (1.0 + d_ppm * 1e-6));
	const std::int64_t q0 = 1'000'000'000;
	const std::int64_t display_d = 100000;
	const std::int64_t tick = 16667;
	std::uint64_t state = seed;
	auto rand01 = [&state] {
		state = state * 6364136223846793005ull + 1442695040888963407ull;
		return double(state >> 11) * (1.0 / 9007199254740992.0);
	};

	AudioMixer mixer;
	const MixerKey screen_key = screen("a");
	const MixerKey voice_key = voice("b");
	PulseFinder screen_pulses(0);
	PulseFinder voice_pulses(1);
	std::int64_t next_take = q0;
	auto take_until = [&](std::int64_t now) {
		while (next_take <= now) {
			for (const MixedBlock &block : mixer.take(next_take)) {
				screen_pulses.feed(block);
				voice_pulses.feed(block);
			}
			next_take += 10000;
		}
	};

	std::vector<float> screen_chunk(480 * 2);
	std::vector<float> voice_chunk(480 * 2);
	const std::int64_t chunks = std::int64_t(minutes * 60.0 * 100.0);
	for (std::int64_t i = 0; i < chunks; ++i) {
		const double play_q = double(q0) + double(i * 480) * sample_q_us;
		std::fill(screen_chunk.begin(), screen_chunk.end(), 0.0f);
		std::fill(voice_chunk.begin(), voice_chunk.end(), 0.0f);
		const std::int64_t first = i * 480;
		const std::int64_t pulse = ((first + 47999) / 48000) * 48000;
		if (pulse < first + 480) {
			screen_chunk[std::size_t(pulse - first) * 2] = 1.0f;
			voice_chunk[std::size_t(pulse - first) * 2 + 1] = 1.0f;
		}
		// 画面の音: タイムスタンプは取り出した時刻 (+-0.5 ms の揺れ) で、2 から 5 ms 後に届く。
		const std::int64_t ts_obs = std::llround(play_q + (rand01() - 0.5) * 1000.0);
		const std::int64_t screen_arrival = std::llround(play_q + 2000.0 + rand01() * 3000.0);
		take_until(screen_arrival);
		const std::int64_t screen_at =
			cordmux::pipeline::screen_audio_at_us(ts_obs, screen_arrival, display_d, tick, 0);
		mixer.add(screen_arrival, screen_key, screen_at, screen_chunk.data(), 480);
		// 声: 届いた時刻から置く (1 から 4 ms 後に届く)。
		const std::int64_t voice_arrival = std::llround(play_q + 1000.0 + rand01() * 3000.0);
		take_until(voice_arrival);
		mixer.add(voice_arrival, voice_key, cordmux::pipeline::voice_audio_at_us(voice_arrival, 0), voice_chunk.data(),
			  480);
	}
	take_until(q0 + std::int64_t(minutes * 60e6) + 500000);

	const AudioMixerStats stats = mixer.take_stats();
	MixerRunResult r;
	r.skips = stats.skips;
	for (const AudioMixerKeyStats &k : stats.keys) {
		r.late_frames += k.late_frames;
		if (k.key == screen_key) {
			r.screen_replaced = k.replaced;
			r.screen_drift_ppm = k.drift_ppm;
		}
		if (k.key == voice_key)
			r.voice_replaced = k.replaced;
	}
	// m 番目のパルスは 48000 m 番目のサンプルで、Q の q0 + 48000 m x sample_q_us に取り出された。
	auto measure = [&](const std::vector<double> &pulses, double expected_offset_us, double *drift, double *step) {
		std::optional<double> start;
		double prev = 0.0;
		bool has_prev = false;
		for (std::size_t m = 0; m < pulses.size(); ++m) {
			const double expected = double(q0) + double(m) * 48000.0 * sample_q_us + expected_offset_us;
			const double err = pulses[m] - expected;
			if (m >= 60 && !start)
				start = err;
			if (has_prev)
				*step = std::max(*step, std::fabs(err - prev));
			prev = err;
			has_prev = true;
			if (start)
				*drift = err - *start;
		}
	};
	measure(screen_pulses.pulses, double(display_d + tick / 2), &r.screen_drift_us, &r.screen_max_step_us);
	measure(voice_pulses.pulses, double(cordmux::pipeline::kVoiceMixJitterUs) + 2500.0, &r.voice_drift_us,
		&r.voice_max_step_us);
	r.screen_pulses = screen_pulses.pulses.size();
	r.voice_pulses = voice_pulses.pulses.size();
	return r;
}

} // namespace

TEST_CASE("実物の AudioMixer に音の時計が +-500 ppm ずれたチャンクを 20 分流しても、出した音の目印の時刻は時間とともにずれず、置き直さない")
{
	std::uint64_t seed = 3;
	std::printf("\n| AudioMixer の模擬 | パルス (画面/声) | 画面: 終わり-始め (ms) | 画面: 最大の飛び (ms) | 声: 終わり-始め (ms) | "
		    "声: 最大の飛び (ms) | 置き直し (画面/声) | 遅れて捨てた | 推定した速さの差 (ppm) |\n");
	std::printf("|---|---|---|---|---|---|---|---|---|\n");
	for (const double d_ppm : {500.0, -500.0}) {
		CAPTURE(d_ppm);
		const MixerRunResult r = run_mixer_long(d_ppm, 20.0, seed++);
		std::printf("| D %+.0f ppm、20 分 | %zu/%zu | %.3f | %.3f | %.3f | %.3f | %llu/%llu | %llu | %.1f |\n", d_ppm,
			    r.screen_pulses, r.voice_pulses, r.screen_drift_us / 1000.0, r.screen_max_step_us / 1000.0,
			    r.voice_drift_us / 1000.0, r.voice_max_step_us / 1000.0,
			    static_cast<unsigned long long>(r.screen_replaced), static_cast<unsigned long long>(r.voice_replaced),
			    static_cast<unsigned long long>(r.late_frames), r.screen_drift_ppm);
		CHECK(r.screen_pulses >= 1199);
		CHECK(r.voice_pulses >= 1199);
		// 時計の差 (20 分で 600 ms) は、どちらの組でも出した音の時刻に残らない。
		CHECK(std::fabs(r.screen_drift_us) < 2000.0);
		CHECK(std::fabs(r.voice_drift_us) < 2000.0);
		CHECK(r.screen_max_step_us < 5000.0);
		CHECK(r.voice_max_step_us < 5000.0);
		CHECK(r.screen_replaced == 0);
		CHECK(r.voice_replaced == 0);
		CHECK(r.late_frames == 0);
		CHECK(r.skips == 0);
		// 推定した速さの差は、音の時計の速さの差と逆の向きになる (出力のサンプルを少なく作る)。
		CHECK(r.screen_drift_ppm == doctest::Approx(-d_ppm / (1.0 + d_ppm * 1e-6)).epsilon(0.05));
	}
}

TEST_CASE("30 組 (声 15、画面の音 15) の音を、組ごとに違う速さの差で置いて混ぜる時間を測る")
{
	// 値は計算機で変わるので合否は決めず、表で出す。配信の 1 秒 (置くのは各組 100 回、取り出すのは 100 回) にかかる
	// 時間から、1 コアに占める割合を出す。組ごとの速さの差を -300 から +300 ppm に散らす場合 (変換をする) と、
	// どの組も 0 の場合 (変換をしない) を比べる。
	std::vector<MixerKey> keys;
	std::vector<std::string> names;
	for (int k = 0; k < 30; ++k)
		names.push_back("u" + std::to_string(k));
	for (int k = 0; k < 30; ++k)
		keys.push_back(MixerKey{names[std::size_t(k)], k < 15 ? MixerKind::Voice : MixerKind::ScreenAudio});
	const std::vector<float> data = chunk(0.01f);
	const int seconds = 60;
	auto run = [&](double spread_ppm) {
		AudioMixer mixer;
		std::size_t blocks = 0;
		const auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < seconds * 100; ++i) {
			const std::int64_t now = kT + std::int64_t(i) * 10000;
			for (int k = 0; k < 30; ++k) {
				const double ppm = spread_ppm * (-1.0 + 2.0 * double(k) / 29.0);
				const std::int64_t at = kT + 100000 + std::llround(double(i) * 10000.0 / (1.0 + ppm * 1e-6));
				mixer.add(now, keys[std::size_t(k)], at, data.data(), 480);
			}
			blocks += mixer.take(now).size();
		}
		const double us =
			std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
		CHECK(blocks > 0);
		const AudioMixerStats stats = mixer.take_stats();
		for (const AudioMixerKeyStats &k : stats.keys)
			CHECK(k.replaced == 0);
		return us / double(seconds);
	};
	const double converting_us = run(300.0);
	const double plain_us = run(0.0);
	std::printf("\n| AudioMixer に 30 組を置いて混ぜる | 配信の 1 秒あたりの時間 | 1 コアに占める割合 |\n");
	std::printf("|---|---|---|\n");
	std::printf("| 変換する (-300 から +300 ppm) | %.0f us | %.2f %% |\n", converting_us, converting_us / 1e6 * 100.0);
	std::printf("| 変換しない (0 ppm) | %.0f us | %.2f %% |\n", plain_us, plain_us / 1e6 * 100.0);
}

TEST_CASE("声の組は、画面の音より広い置き直しのしきい値 (既定 70 ms) を使う")
{
	AudioMixer mixer(no_skip_config());
	CHECK(mixer.config().voice_resync_us == 70000);
	const MixerKey v = voice("v");
	const MixerKey s = screen("s");
	add_chunk(mixer, kT, v, kT + 20000, 1.0f);
	add_chunk(mixer, kT, s, kT + 20000, 1.0f);
	// 続き (30 ms) より 60 ms 後ろ。声はしきい値 (70 ms) の内なので置き直さず、画面の音 (45 ms) は置き直す。
	add_chunk(mixer, kT, v, kT + 30000 + 60000, 2.0f);
	add_chunk(mixer, kT, s, kT + 30000 + 60000, 2.0f);
	const AudioMixerStats stats = mixer.take_stats();
	REQUIRE(find_stats(stats, v) != nullptr);
	REQUIRE(find_stats(stats, s) != nullptr);
	CHECK(find_stats(stats, v)->replaced == 0);
	CHECK(find_stats(stats, s)->replaced == 1);
}
