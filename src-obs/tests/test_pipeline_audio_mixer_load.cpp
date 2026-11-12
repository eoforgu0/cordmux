// SPDX-License-Identifier: Apache-2.0

// pipeline/audio_mixer.h (AudioMixer) の処理の重さを、声の人数を変えて測る。声の枠に上限がなくなったので、30 人が同時に
// 話したときに、1 秒分の音を混ぜる時間が人数に比例して増える範囲に収まるかを見るためのもので、合否は持たない。
// 環境変数 CORDMUX_MIXER_LOAD_OUT にファイルのパスがあるときだけ測り、結果をそこにタブ区切りで書く (標準出力にも出す)。
// 普通の実行では何もせずに通る (通常のテストの時間を延ばさない)。
//
// 測るもの (どれも libobs なしで、実際の処理と同じ関数を呼ぶ)。
//   - 1 秒分の処理: 全員が 10 ms のチャンクを 100 回ずつ置き、10 ms ごとに take して区間を出す。全員が話し続ける最悪の場合。
//   - 最初の確保: 組ごとの保持の領域 (1 組で約 0.87 MB) を最初のチャンクで確保する時間。
//   - 500 ms ごとの処理: levels() のコピーと take_stats()。ドックのポーリングと統計の周期が呼ぶ。

#include "doctest.h"

#include "pipeline/audio_mixer.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using cordmux::pipeline::AudioMixer;
using cordmux::pipeline::MixerKey;
using cordmux::pipeline::MixerKind;

namespace {

struct LoadResult {
	int voices = 0;
	double first_chunk_ms = 0.0;   // 全員の最初のチャンクを置く時間 (保持の領域の確保を含む)
	double per_second_ms = 0.0;    // 1 秒分の add と take の時間 (定常の 1 秒の平均)
	double levels_us = 0.0;        // levels() 1 回
	double take_stats_us = 0.0;    // take_stats() 1 回
	std::uint64_t blocks = 0;      // 測った間に出した区間の数
	std::uint64_t catch_up = 0;
};

double ms_since(std::chrono::steady_clock::time_point t0)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

LoadResult measure(int voices, int seconds)
{
	LoadResult result;
	result.voices = voices;
	AudioMixer mixer;
	std::vector<float> chunk(480 * 2, 0.25f);
	std::vector<MixerKey> keys;
	for (int i = 0; i < voices; ++i)
		keys.push_back(MixerKey{"voice" + std::to_string(i), MixerKind::Voice});

	std::int64_t now_us = 1'000'000;
	std::int64_t at_us = now_us + 65'000; // 揺れを吸う幅 J のぶん先に置く
	{
		const auto t0 = std::chrono::steady_clock::now();
		for (const MixerKey &key : keys)
			mixer.add(now_us, key, at_us, chunk.data(), 480);
		result.first_chunk_ms = ms_since(t0);
	}
	std::uint64_t blocks = 0;
	const auto t0 = std::chrono::steady_clock::now();
	for (int step = 0; step < seconds * 100; ++step) {
		now_us += 10'000;
		at_us += 10'000;
		for (const MixerKey &key : keys)
			mixer.add(now_us, key, at_us, chunk.data(), 480);
		blocks += mixer.take(now_us).size();
	}
	result.per_second_ms = ms_since(t0) / seconds;
	result.blocks = blocks;

	const int repeat = 200;
	{
		const auto t1 = std::chrono::steady_clock::now();
		std::size_t sink = 0;
		for (int i = 0; i < repeat; ++i)
			sink += mixer.levels().size();
		result.levels_us = ms_since(t1) * 1000.0 / repeat;
		(void)sink;
	}
	{
		const auto t1 = std::chrono::steady_clock::now();
		std::size_t sink = 0;
		for (int i = 0; i < repeat; ++i)
			sink += mixer.take_stats().keys.size();
		result.take_stats_us = ms_since(t1) * 1000.0 / repeat;
		(void)sink;
	}
	result.catch_up = mixer.take_stats().catch_up_blocks;
	return result;
}

} // namespace

TEST_CASE("音声ミキサーの負荷の測定 (声 8 人と 30 人。環境変数があるときだけ測り、合否なし)")
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // getenv は移植性があり、ここはテストだけが使う
#endif
	const char *out = std::getenv("CORDMUX_MIXER_LOAD_OUT");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
	if (out == nullptr || out[0] == 0)
		return;
	std::string text =
		"voices\tfirst_chunk_ms\tms_per_audio_second\tcpu_percent_of_one_core\tlevels_us\ttake_stats_us\tblocks\n";
	const int counts[] = {8, 16, 30};
	for (const int voices : counts) {
		measure(voices, 2); // 暖機 (最初の確保の影響を、測る前に済ませる)
		const LoadResult r = measure(voices, 10);
		char line[256];
		std::snprintf(line, sizeof(line), "%d\t%.3f\t%.3f\t%.2f\t%.2f\t%.2f\t%llu\n", r.voices, r.first_chunk_ms,
			      r.per_second_ms, r.per_second_ms / 10.0, r.levels_us, r.take_stats_us,
			      static_cast<unsigned long long>(r.blocks));
		text += line;
	}
	std::printf("%s", text.c_str());
	std::ofstream file(out, std::ios::binary | std::ios::trunc);
	file << text;
}
