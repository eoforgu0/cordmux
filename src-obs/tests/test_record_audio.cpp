// SPDX-License-Identifier: Apache-2.0

// record/record_audio.h の単体テスト。録画の音声を書く位置の計算を確認する。
// ワイヤー形式の音声は 48 kHz、2 チャンネルで、1 つのチャンクは 480 サンプル (10 ms) である。

#include "doctest.h"

#include "record/record_audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

using cordmux::record::audio_cursor_end_us;
using cordmux::record::audio_samples_to_us;
using cordmux::record::audio_target_pos;
using cordmux::record::AudioCursor;
using cordmux::record::AudioPlacement;
using cordmux::record::place_audio;
using cordmux::record::silence_samples_for;

namespace {

constexpr int kRate = 48000;
constexpr std::size_t kChunk = 480; // 10 ms

// 置き直しのしきい値は既定の 45 ms (設定の audio_resync_ms)。
cordmux::align::AudioDriftConfig drift_config()
{
	return cordmux::align::make_audio_drift_config(45, 1000, 8000);
}

// チャンクを 1 つ置く。書くサンプルは out に入る。
AudioPlacement push(AudioCursor &cursor, std::int64_t pts_us, std::int64_t origin_us, std::vector<float> *out,
		    float value = 0.5f)
{
	std::vector<float> in(kChunk * 2, value);
	return place_audio(cursor, pts_us, origin_us, in.data(), kChunk, out);
}

} // namespace

TEST_CASE("セグメントの最初のチャンクは位置 0 に置き、受け取ったサンプルをそのまま書く")
{
	AudioCursor cursor(drift_config());
	std::vector<float> out;
	const AudioPlacement first = push(cursor, 1'234'567, 1'234'567, &out);
	CHECK_FALSE(first.backward);
	CHECK_FALSE(first.resynced);
	CHECK(first.silence_samples == 0);
	CHECK(first.frames == kChunk);
	CHECK(out == std::vector<float>(kChunk * 2, 0.5f));
	CHECK(cursor.written_samples == std::int64_t(kChunk));
	CHECK(audio_cursor_end_us(cursor, 1'234'567) == 1'234'567 + 10'000);
}

TEST_CASE("10 ms ごとのタイムスタンプのとおりに届いたチャンクは、変換せずに続けて書く")
{
	AudioCursor cursor(drift_config());
	std::vector<float> out;
	for (int i = 0; i < 100; ++i) {
		const AudioPlacement placement = push(cursor, 1'000'000 + i * 10'000, 1'000'000, &out);
		CHECK(placement.silence_samples == 0);
		CHECK_FALSE(placement.resynced);
		CHECK(placement.frames == kChunk);
	}
	CHECK(cursor.written_samples == std::int64_t(kChunk) * 100);
	CHECK(audio_cursor_end_us(cursor, 1'000'000) == 1'000'000 + 1'000'000);
}

TEST_CASE("しきい値の内のずれでは置き直さず、書くサンプルの数を少し変えて追う")
{
	AudioCursor cursor(drift_config());
	std::vector<float> out;
	push(cursor, 0, 0, &out);
	// 続きは 10 ms。44 ms 遅れて届いた。
	const AudioPlacement placement = push(cursor, 10'000 + 44'000, 0, &out);
	CHECK_FALSE(placement.resynced);
	CHECK_FALSE(placement.backward);
	CHECK(placement.silence_samples == 0);
	// 書き続けるので、書いた数は 0 のサンプルなしで続く。
	CHECK(cursor.written_samples == std::int64_t(kChunk) + std::int64_t(placement.frames));
}

TEST_CASE("しきい値を超えて遅れて届いたら、0 のサンプルで埋めて目標の位置に置き直す")
{
	AudioCursor cursor(drift_config());
	std::vector<float> out;
	push(cursor, 0, 0, &out);
	push(cursor, 10'000, 0, &out);
	// 1 秒分が欠けた。続き (20 ms) より 1 秒以上遅いので置き直す。
	const std::int64_t written_before = cursor.written_samples;
	CHECK(written_before == 960);
	const AudioPlacement big = push(cursor, 1'060'000, 0, &out);
	CHECK(big.resynced);
	CHECK(big.drift_us > 45'000);
	CHECK(big.silence_samples == std::llround(audio_target_pos(1'060'000, 0, kRate)) - written_before);
	CHECK(big.frames == kChunk);
	CHECK(cursor.written_samples == std::llround(audio_target_pos(1'060'000, 0, kRate)) + std::int64_t(kChunk));
	// 次のチャンクは続きに置く。
	const AudioPlacement next = push(cursor, 1'070'000, 0, &out);
	CHECK_FALSE(next.resynced);
	CHECK(next.silence_samples == 0);
}

TEST_CASE("しきい値を超えて前へ戻ったチャンクは、書いたサンプルを消せないので、カーソルを変えずに backward を返す")
{
	AudioCursor cursor(drift_config());
	std::vector<float> out;
	push(cursor, 1'000'000, 1'000'000, &out);
	push(cursor, 1'010'000, 1'000'000, &out);
	const std::int64_t written_before = cursor.written_samples;
	// 続きは 1'020'000。100 ms 前の値が届いた。
	const AudioPlacement placement = push(cursor, 920'000, 1'000'000, &out);
	CHECK(placement.backward);
	CHECK(placement.drift_us == -100'000);
	CHECK(placement.frames == 0);
	CHECK(out.empty());
	CHECK(cursor.written_samples == written_before);
	// カーソルは変わっていないので、続きのチャンクはそのまま続けて書ける。
	const AudioPlacement next = push(cursor, 1'020'000, 1'000'000, &out);
	CHECK_FALSE(next.backward);
	CHECK_FALSE(next.resynced);
}

TEST_CASE("しきい値の内なら、前へ戻ったチャンクも置き直さずに続けて書く")
{
	AudioCursor cursor(drift_config());
	std::vector<float> out;
	push(cursor, 0, 0, &out);
	const AudioPlacement placement = push(cursor, 10'000 - 30'000, 0, &out);
	CHECK_FALSE(placement.backward);
	CHECK_FALSE(placement.resynced);
	CHECK(placement.silence_samples == 0);
}

TEST_CASE("欠けたチャンクの分だけ無音を入れ、欠落の長さをそのままサンプル数に修正する")
{
	CHECK(silence_samples_for(1'000'000, kRate) == 48000);
	CHECK(silence_samples_for(10'000, kRate) == 480);
	CHECK(silence_samples_for(0, kRate) == 0);
	CHECK(silence_samples_for(-5, kRate) == 0);
	CHECK(silence_samples_for(1'000'000, 0) == 0);
	// 端数は最も近い整数に丸める。
	CHECK(silence_samples_for(21, kRate) == 1);
	CHECK(silence_samples_for(10, kRate) == 0);
}

TEST_CASE("サンプル数を長さに修正し、長さをサンプル数に戻せる")
{
	CHECK(audio_samples_to_us(48000, kRate) == 1'000'000);
	CHECK(audio_samples_to_us(480, kRate) == 10'000);
	CHECK(audio_samples_to_us(0, kRate) == 0);
	CHECK(audio_samples_to_us(480, 0) == 0);
	CHECK(audio_target_pos(1'010'000, 1'000'000, kRate) == doctest::Approx(480.0));
}

// ---------------------------------------------------------------------------
// 長い録画の模擬
// ---------------------------------------------------------------------------
//
// 実物の place_audio に、音の時計 (サンプルを取り出す速さ) がタイムスタンプの時計より d_ppm 速い (負なら遅い) チャンクを
// 20 分流す。10 時間の +-100 ppm (3.6 秒) と同じ桁の量 (500 ppm で 20 分 = 0.6 秒) を短い時間で起こす。チャンクの
// タイムスタンプは、その先頭のサンプルを取り出した時刻 (+-0.5 ms の揺れ) である。途中で 3 秒分のチャンクが届かず
// (前向きの置き直しと 0 埋め)、その後でタイムスタンプが 300 ms 前へ飛ぶ (後ろ向きの置き直し。録画は
// recorder.cpp の handle_audio と同じく、セグメントを閉じて新しいカーソルで置き直す)。音には 1 秒 (48000 サンプル)
// ごとに目印のパルスを入れ、書いたサンプルの中のパルスの時刻 (原点 + 書いた位置の長さ) を、そのサンプルを本当に
// 取り出した時刻と比べる。

namespace {

struct RecordRunResult {
	double drift_us = 0.0;    // パルスの時刻の誤差: 最後 - 60 秒の時点
	double max_step_us = 0.0; // 隣り合うパルスの誤差の差の最大
	std::size_t pulses = 0;
	int resyncs = 0;
	int backwards = 0;
	std::int64_t gap_silence = 0;     // 欠けたチャンクの後に入れた 0 のサンプル数
	std::int64_t gap_samples = 0;     // 欠けた分の長さ (出力のサンプル数)
	double end_error_us = 0.0;        // 各セグメントの終わりの時刻 (audio_cursor_end_us) - 最後のチャンクの終わりの本当の時刻 の最大
	double drift_ppm = 0.0;
};

RecordRunResult run_record_long(double d_ppm, double minutes, std::uint64_t seed)
{
	const double sample_ts_us = 1e6 / (double(kRate) * (1.0 + d_ppm * 1e-6)); // 1 サンプルのタイムスタンプの時計での長さ
	const double pulse_period_us = 48000.0 * sample_ts_us;
	const std::int64_t ts0 = 5'000'000'000;
	const std::int64_t chunks = std::int64_t(minutes * 60.0 * 100.0);
	const std::int64_t gap_first = chunks / 2;
	const std::int64_t gap_chunks = 300;
	const std::int64_t jump_first = chunks * 3 / 4;
	const std::int64_t jump_us = -300'000;
	std::uint64_t state = seed;
	auto rand01 = [&state] {
		state = state * 6364136223846793005ull + 1442695040888963407ull;
		return double(state >> 11) * (1.0 / 9007199254740992.0);
	};

	RecordRunResult r;
	std::optional<AudioCursor> cursor;
	std::int64_t origin = 0;
	std::int64_t shift_us = 0;         // そのときのタイムスタンプの飛び
	double segment_true_end_us = 0.0;  // 開いているセグメントに書いた最後のチャンクの終わりの本当の時刻 + 飛び
	std::vector<float> in(kChunk * 2);
	std::vector<float> out;
	bool in_peak = false;
	float peak = 0.0f;
	double peak_t = 0.0;
	std::optional<double> start;
	std::optional<double> prev;
	auto close_segment = [&] {
		if (cursor)
			r.end_error_us = std::max(r.end_error_us,
						  std::fabs(double(audio_cursor_end_us(*cursor, origin)) - segment_true_end_us));
	};
	auto note_pulse = [&](double t) {
		// 何番目のパルスかは、誤差がパルスの間隔の半分より小さいので、時刻から決まる。
		const double from0 = t - double(shift_us) - double(ts0);
		const double m = std::round(from0 / pulse_period_us);
		const double err = from0 - m * pulse_period_us;
		++r.pulses;
		if (prev)
			r.max_step_us = std::max(r.max_step_us, std::fabs(err - *prev));
		prev = err;
		if (!start && from0 >= 60e6)
			start = err;
		if (start)
			r.drift_us = err - *start;
	};
	for (std::int64_t i = 0; i < chunks; ++i) {
		if (i >= gap_first && i < gap_first + gap_chunks)
			continue;
		if (i == jump_first)
			shift_us = jump_us;
		const double true_us = double(ts0) + double(i * std::int64_t(kChunk)) * sample_ts_us;
		std::fill(in.begin(), in.end(), 0.0f);
		const std::int64_t first = i * std::int64_t(kChunk);
		const std::int64_t pulse = ((first + 47999) / 48000) * 48000;
		if (pulse < first + std::int64_t(kChunk))
			in[std::size_t(pulse - first) * 2] = 1.0f;
		const std::int64_t pts = std::llround(true_us + (rand01() - 0.5) * 1000.0) + shift_us;
		if (!cursor) {
			cursor.emplace(drift_config());
			origin = pts;
		}
		AudioPlacement placement = place_audio(*cursor, pts, origin, in.data(), kChunk, &out);
		if (placement.backward) {
			++r.backwards;
			close_segment();
			cursor.emplace(drift_config());
			origin = pts;
			in_peak = false;
			placement = place_audio(*cursor, pts, origin, in.data(), kChunk, &out);
		}
		if (placement.resynced) {
			++r.resyncs;
			r.gap_silence = placement.silence_samples;
			r.gap_samples = std::llround(double(gap_chunks * std::int64_t(kChunk)) * sample_ts_us * double(kRate) / 1e6);
		}
		const std::int64_t written_first = cursor->written_samples - std::int64_t(placement.frames);
		for (std::size_t j = 0; j < placement.frames; ++j) {
			const float v = out[j * 2];
			const double t = double(origin) + double(written_first + std::int64_t(j)) * 1e6 / double(kRate);
			if (v > 0.2f) {
				if (!in_peak || v > peak) {
					peak = v;
					peak_t = t;
				}
				in_peak = true;
			} else if (in_peak) {
				note_pulse(peak_t);
				in_peak = false;
				peak = 0.0f;
			}
		}
		segment_true_end_us = true_us + double(kChunk) * sample_ts_us + double(shift_us);
	}
	close_segment();
	r.drift_ppm = cursor->corrector.stats().drift_ppm;
	return r;
}

} // namespace

TEST_CASE("音の時計がタイムスタンプより +-500 ppm ずれたチャンクを 20 分置いても、書いた音の目印の時刻は時間とともにずれず、"
	  "欠けた分は 0 で埋め、前へ戻ったらセグメントを分ける")
{
	std::uint64_t seed = 11;
	std::printf("\n| 録画の模擬 | パルス | 終わり-始め (ms) | 最大の飛び (ms) | 前向きの置き直し | 後ろ向き | "
		    "欠けた分 / 0 埋め (サンプル) | セグメントの終わりの誤差 (ms) | 推定した速さの差 (ppm) |\n");
	std::printf("|---|---|---|---|---|---|---|---|---|\n");
	for (const double d_ppm : {500.0, -500.0}) {
		CAPTURE(d_ppm);
		const RecordRunResult r = run_record_long(d_ppm, 20.0, seed++);
		std::printf("| 音の時計 %+.0f ppm、20 分 | %zu | %.3f | %.3f | %d | %d | %lld / %lld | %.3f | %.1f |\n", d_ppm,
			    r.pulses, r.drift_us / 1000.0, r.max_step_us / 1000.0, r.resyncs, r.backwards,
			    static_cast<long long>(r.gap_samples), static_cast<long long>(r.gap_silence),
			    r.end_error_us / 1000.0, r.drift_ppm);
		// 20 分 (1200 個) から、欠けた 3 秒の 3 個を除いた数。
		CHECK(r.pulses >= 1196);
		// 時計の差 (20 分で 600 ms) は、書いた音の時刻に残らない。
		CHECK(std::fabs(r.drift_us) < 2000.0);
		CHECK(r.max_step_us < 5000.0);
		// 置き直しは、欠けた 3 秒の後の 1 回 (前向き) と、300 ms 前へ飛んだときの 1 回 (後ろ向き) だけ。
		CHECK(r.resyncs == 1);
		CHECK(r.backwards == 1);
		// 0 埋めの量は欠けた長さに、タイムスタンプの揺れ (+-0.5 ms = 24 サンプル) と追従の誤差の分を除いて一致する。
		CHECK(std::llabs(r.gap_silence - r.gap_samples) <= 48);
		// セグメントの終わりの時刻 (録画ログの last_pts_us) は、書いた量で数えても本当の時刻から離れない。
		CHECK(r.end_error_us < 2000.0);
		CHECK(r.drift_ppm == doctest::Approx(-d_ppm / (1.0 + d_ppm * 1e-6)).epsilon(0.1));
	}
}
