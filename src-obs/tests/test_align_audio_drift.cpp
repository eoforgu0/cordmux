// SPDX-License-Identifier: Apache-2.0

// align/audio_drift.h (AudioDriftTracker と AudioDriftCorrector) の単体テスト。時刻のそろった入力をそのまま書くこと、
// 置き直しのしきい値、時計の速さの差の吸収、変換したサンプルの値を確かめる。最後に、音を取り出す時計 (D)、送信側と
// OBS の時計 (Q) を別々の速さにした 10 時間の模擬で、配信の経路 (AudioMixer に置く位置) と録画の経路 (書く位置) の
// 両方の、音と映像のずれを確かめる。

#include "doctest.h"

#include "align/audio_drift.h"
#include "align/timestamp_aligner.h"
#include "fixtures.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

using cordmux::align::AudioDriftConfig;
using cordmux::align::AudioDriftCorrector;
using cordmux::align::AudioDriftStep;
using cordmux::align::AudioDriftTracker;
using cordmux::align::kAudioDriftHalfTaps;
using cordmux::align::make_audio_drift_config;
using cordmux::align::TimestampAligner;
using cordmux::tests::Rng;

namespace {

constexpr double kRate = 48000.0;
constexpr std::size_t kChunk = 480;

std::vector<float> stereo_constant(float v, std::size_t frames = kChunk)
{
	std::vector<float> out(frames * 2);
	for (std::size_t i = 0; i < frames; ++i) {
		out[i * 2] = v;
		out[i * 2 + 1] = -v;
	}
	return out;
}

} // namespace

TEST_CASE("時刻のそろった入力は、変換せずに受け取ったサンプルをそのまま続けて書き出す")
{
	AudioDriftCorrector corrector;
	std::vector<float> out;
	std::vector<float> all;
	for (int i = 0; i < 50; ++i) {
		std::vector<float> in(kChunk * 2);
		for (std::size_t f = 0; f < kChunk; ++f) {
			in[f * 2] = float(i * 1000 + int(f));
			in[f * 2 + 1] = -float(i * 1000 + int(f));
		}
		const AudioDriftStep step = corrector.push(1000.0 + double(i) * kChunk, in.data(), kChunk, &out);
		CHECK(step.resynced == (i == 0));
		CHECK(step.start_pos == 1000 + std::int64_t(i) * std::int64_t(kChunk));
		REQUIRE(step.frames == kChunk);
		CHECK(out == in);
	}
	CHECK(corrector.stats().resyncs == 0);
	CHECK(corrector.stats().rate_ppm == 0.0);
}

TEST_CASE("マイクロ秒に丸めた時刻の端数 (半サンプル未満) では、速さを変えない")
{
	AudioDriftTracker tracker;
	for (int i = 0; i < 1000; ++i) {
		// 441 フレームのチャンクの時刻をマイクロ秒に丸めると、目標が 0.024 サンプルまでずれる。
		const std::int64_t at_us = (std::int64_t(i) * 441 * 1000000 + 24000) / 48000;
		const AudioDriftStep step = tracker.push(double(at_us) * kRate / 1e6, 441);
		REQUIRE(step.frames == 441);
	}
	CHECK(tracker.stats().rate_ppm == 0.0);
	CHECK(tracker.stats().resyncs == 0);
}

TEST_CASE("目標と続きの位置の差がしきい値を超えたら置き直し、しきい値の内なら置き直さずに速さで戻す")
{
	AudioDriftConfig config;
	config.resync_us = 45000;
	AudioDriftTracker tracker(config);
	tracker.push(0.0, kChunk);
	// 続きは 480。44.9 ms (2155 サンプル) 後ろは、しきい値の内。
	const AudioDriftStep inside = tracker.push(480.0 + 2155.0, kChunk);
	CHECK_FALSE(inside.resynced);
	CHECK(inside.error_us == doctest::Approx(2155.0 / 0.048));
	CHECK(tracker.stats().rate_ppm > 0.0); // 後ろへ追うので、出力を少し長くする
	CHECK(tracker.stats().resyncs == 0);
	// 続きから 46 ms (2208 サンプル) 前は、しきい値の外なので置き直す。
	const double next = tracker.next_input_pos();
	const AudioDriftStep outside = tracker.push(next - 2208.0, kChunk);
	CHECK(outside.resynced);
	CHECK(outside.start_pos == std::llround(next - 2208.0));
	CHECK(tracker.stats().resyncs == 1);
}

TEST_CASE("reset の後は必ず目標に置き直すが、置き直しの回数には数えず、推定した速さの差は残す")
{
	AudioDriftTracker tracker;
	// 音の時計が 200 ppm 速い入力を 60 秒続けて、速さの差を推定させる。
	for (int i = 0; i < 6000; ++i)
		tracker.push(double(i) * double(kChunk) / (1.0 + 200e-6), kChunk);
	const double drift = tracker.stats().drift_ppm;
	CHECK(drift == doctest::Approx(-200.0).epsilon(0.05));
	tracker.reset();
	const AudioDriftStep step = tracker.push(123456.0, kChunk);
	CHECK(step.resynced);
	CHECK(step.start_pos == 123456);
	CHECK(tracker.stats().resyncs == 0);
	CHECK(tracker.stats().drift_ppm == doctest::Approx(drift));
}

TEST_CASE("続けて書くと書き出してよい位置より前になり、目標が後ろにあるときは置き直す")
{
	AudioDriftTracker tracker;
	tracker.push(0.0, kChunk);
	// 続きは 480。目標は 1000 (しきい値の内) だが、書き出してよいのは 700 から。
	const AudioDriftStep step = tracker.push(1000.0, kChunk, std::int64_t(700));
	CHECK(step.resynced);
	CHECK(step.start_pos == 1000);
	CHECK(tracker.stats().resyncs == 1);
	// 目標も前にあるときは、置き直しても間に合わないので続けて書く。
	const double next = tracker.next_input_pos();
	const AudioDriftStep late = tracker.push(next - 10.0, kChunk, std::int64_t(next + 100.0));
	CHECK_FALSE(late.resynced);
}

TEST_CASE("音の時計の速さの差は、積分が持って位置の差が 0 に戻り、補正は上限を超えない")
{
	for (const double d_ppm : {100.0, -100.0, 900.0}) {
		CAPTURE(d_ppm);
		AudioDriftTracker tracker;
		double max_rate = 0.0;
		double last_error_us = 0.0;
		// 音の時計が d_ppm 速いと、1 チャンク (480 サンプル) は出力の時計で 480 / (1 + d) サンプルの長さになる。
		for (int i = 0; i < 30000; ++i) {
			const double target = double(i) * double(kChunk) / (1.0 + d_ppm * 1e-6);
			const AudioDriftStep step = tracker.push(target, kChunk);
			max_rate = std::max(max_rate, std::fabs(tracker.stats().rate_ppm));
			last_error_us = step.error_us;
		}
		CHECK(tracker.stats().resyncs == 0);
		CHECK(max_rate <= 1000.0 + 1e-9);
		CHECK(std::fabs(last_error_us) < 50.0);
		CHECK(tracker.stats().drift_ppm == doctest::Approx(-d_ppm).epsilon(0.02));
	}
}

TEST_CASE("変換したサンプルは、入力の正弦波を書き出した位置で読んだ値に一致する")
{
	// 出力の時計が入力より 500 ppm 速い (出力のサンプルを多く作る) とき、1 kHz と 10 kHz の正弦波を通す。
	for (const double freq : {1000.0, 10000.0}) {
		CAPTURE(freq);
		AudioDriftCorrector corrector;
		const double ratio = 1.0 + 500e-6;
		std::vector<float> out;
		double max_err = 0.0;
		std::int64_t total_in = 0;
		for (int i = 0; i < 6000; ++i) {
			std::vector<float> in(kChunk * 2);
			for (std::size_t f = 0; f < kChunk; ++f) {
				const double n = double(total_in + std::int64_t(f));
				const double v = 0.5 * std::sin(2.0 * 3.14159265358979323846 * freq * n / kRate);
				in[f * 2] = float(v);
				in[f * 2 + 1] = float(-v);
			}
			const AudioDriftStep step = corrector.push(double(total_in) * ratio, in.data(), kChunk, &out);
			total_in += std::int64_t(kChunk);
			// 書き出した j 番目のサンプルは、入力の first + j x 進み の位置にある。
			const std::int64_t xi = corrector.tracker().first_input_int();
			const double xf = corrector.tracker().first_input_frac();
			const double inc = corrector.tracker().input_step();
			for (std::size_t j = 0; j < step.frames; ++j) {
				const double x = double(xi) + xf + double(j) * inc;
				// 速さの差を推定し終えた後 (40 秒) で比べる。入力の 0 番目より前を含む最初の部分は除く。
				if (i > 4000 && x > double(kAudioDriftHalfTaps)) {
					const double want = 0.5 * std::sin(2.0 * 3.14159265358979323846 * freq * x / kRate);
					max_err = std::max(max_err, std::fabs(double(out[j * 2]) - want));
					max_err = std::max(max_err, std::fabs(double(out[j * 2 + 1]) + want));
				}
			}
		}
		CHECK(corrector.stats().drift_ppm == doctest::Approx(500.0).epsilon(0.05));
		// 振幅 0.5 に対して -60 dB (0.0005) より小さい。
		CHECK(max_err < 0.0005);
	}
}

TEST_CASE("一定の値のチャンクを速さを変えながら書き出しても、値は一定のまま変わらない")
{
	AudioDriftCorrector corrector;
	std::vector<float> out;
	const std::vector<float> in = stereo_constant(0.25f);
	for (int i = 0; i < 500; ++i) {
		const double target = double(i) * double(kChunk) * (1.0 + 800e-6);
		const AudioDriftStep step = corrector.push(target, in.data(), kChunk, &out);
		if (i < 2)
			continue; // 最初のチャンクは、入力の 0 番目より前の無音を含む
		for (std::size_t j = 0; j < step.frames; ++j) {
			REQUIRE(out[j * 2] == doctest::Approx(0.25f).epsilon(1e-4));
			REQUIRE(out[j * 2 + 1] == doctest::Approx(-0.25f).epsilon(1e-4));
		}
	}
}

TEST_CASE("480 サンプルのチャンクを 1 つ置く時間を測る (変換をするときと、しないとき)")
{
	// 値は計算機で変わるので合否は決めず、表で出す。配信の組 1 つは 1 秒に 100 回置くので、30 組で 1 コアに占める割合は
	// 30 x 100 x (1 回の時間) で見積もる。
	std::printf("\n| 1 組のチャンク (480 サンプル) を置く | 1 回の時間 | 30 組で 1 コアに占める割合 |\n");
	std::printf("|---|---|---|\n");
	for (const double d_ppm : {300.0, 0.0}) {
		CAPTURE(d_ppm);
		AudioDriftCorrector corrector;
		const std::vector<float> in = stereo_constant(0.25f);
		std::vector<float> out;
		const double step = double(kChunk) / (1.0 + d_ppm * 1e-6);
		double target = 0.0;
		for (int i = 0; i < 2000; ++i) {
			corrector.push(target, in.data(), kChunk, &out);
			target += step;
		}
		const int n = 30000;
		std::size_t frames = 0;
		const auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < n; ++i) {
			frames += corrector.push(target, in.data(), kChunk, &out).frames;
			target += step;
		}
		const double us =
			std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / double(n);
		std::printf("| %s | %.2f us | %.2f %% |\n", d_ppm != 0.0 ? "変換する (300 ppm)" : "変換しない", us,
			    30.0 * 100.0 * us / 1e6 * 100.0);
		// 書き出す数は、入力の数を速さの比で割った数になる。
		CHECK(double(frames) == doctest::Approx(double(n) * double(kChunk) / (1.0 + d_ppm * 1e-6)).epsilon(1e-4));
	}
}

TEST_CASE("設定の値から作る追従の値は、0 以下を 1 として扱う")
{
	const AudioDriftConfig c = make_audio_drift_config(45, 1000, 8000);
	CHECK(c.resync_us == 45000);
	CHECK(c.max_ppm == 1000.0);
	CHECK(c.time_constant_us == 8000000);
	const AudioDriftConfig z = make_audio_drift_config(0, -3, 0);
	CHECK(z.resync_us == 1000);
	CHECK(z.max_ppm == 1.0);
	CHECK(z.time_constant_us == 1000);
}

// ---------------------------------------------------------------------------
// 10 時間の模擬
// ---------------------------------------------------------------------------
//
// 時計は 2 つを動かす。Q は Windows の単調な時計で、送信側のタイムスタンプも OBS の時計もこれで進む (送信側の時計は
// Q + 定数、OBS の時計は Q そのもの)。D は Vesktop の PC で受信した音を取り出す時計で、Q より d_ppm 速い。システム
// 時刻 (S) は、送信側でも OBS 側でも時刻付けの入力にならないので、この模擬には出てこない。S を +-10 ppm で進めて
// 1 秒飛ばしても送信側のタイムスタンプが動かないことは、送信側のテスト (src-vencord/tests/encode_sender_clock.test.ts
// と src-vencord/tests/encode_clock_map.test.ts の 10 時間の模擬) で確かめる。
//
// 中身の時刻 c (音と映像の素材が撮られた時刻) を軸にする。音の n 番目のサンプルは c = n / (48000 (1 + d)) で、
// Q の c + La + up_a(c) に再生され、その時刻 (と揺れ) が AudioData.timestamp になる。映像の k 番目のフレームは
// c = k / 60 で、Q の c + Lv + up_v(c) が VideoFrame.timestamp になる。送信側はどちらにも定数を足して送る。
// 配信の経路は、映像を TimestampAligner で整列して D を足した時刻に出し、音は直近の映像のオフセットと D と半刻みを
// 足した at_us を、AudioMixer と同じ形で AudioDriftTracker に渡す。録画の経路は、映像を送信側のタイムスタンプのまま
// 置き、音は (タイムスタンプ - 原点) の位置を AudioDriftTracker に渡す。どちらも、音のチャンクの最初のサンプルが
// 実際に置かれた時刻 - 同じ中身の時刻の映像が出る時刻 を、音と映像のずれとして測る。配信の映像の時刻は、整列が
// 出した前後のフレームの時刻から、中身の時刻で補って求める。

namespace {

struct LongRun {
	const char *name = "";
	double d_ppm = 0.0;           // D の Q に対する速さの差
	double ts_jitter_us = 1000.0; // AudioData.timestamp の揺れ (一様、+-)
	int device_chunks = 1;        // 音の装置が 1 回に取り出すチャンクの数 (2 なら 2 つのチャンクがほぼ同じ時刻を持つ)
	bool audio_steps = false;     // 音だけが動く上流の段 (2 時間目から 1 時間 +15 ms、4 時間目から 30 分 -8 ms)
	bool video_steps = false;     // 映像だけが動く上流の段 (5 時間目から 1 時間 -17 ms)
	bool receive_gap = false;     // 7 時間目に 3 秒の受信の途切れを入れる
	double hours = 10.0;
};

struct PathResult {
	std::optional<double> start; // 測り始め (60 秒) のずれ (us)
	double end_us = 0.0;         // 最後のずれ
	double max_jump_us = 0.0;    // 置き直したチャンクを除く、隣り合うチャンクのずれの差の最大
	double max_dev_us = 0.0;     // 上流の段と途切れの前後 2 分を除いた、ずれ - 測り始めのずれ の最大
	std::uint64_t resyncs = 0;
};

struct LongRunResult {
	PathResult mix;
	PathResult rec;
};

constexpr double kHour = 3600.0;

double upstream_audio(const LongRun &run, double c)
{
	if (!run.audio_steps)
		return 0.0;
	if (c >= 2 * kHour && c < 3 * kHour)
		return 0.015;
	if (c >= 4 * kHour && c < 4.5 * kHour)
		return -0.008;
	return 0.0;
}

double upstream_video(const LongRun &run, double c)
{
	return run.video_steps && c >= 5 * kHour && c < 6 * kHour ? -0.017 : 0.0;
}

bool in_gap(const LongRun &run, double c)
{
	return run.receive_gap && c >= 7 * kHour && c < 7 * kHour + 3.0;
}

// 上流の段と途切れの後の 2 分は、ずれが動いてよい区間 (戻すのに時間がかかる) として、最大のずれに数えない。段のある
// 区間の中も数えない (上流が音と映像の関係を変えている)。
bool settled(const LongRun &run, double c)
{
	if (upstream_audio(run, c) != 0.0 || upstream_video(run, c) != 0.0)
		return false;
	const double edges[] = {2 * kHour, 3 * kHour, 4 * kHour, 4.5 * kHour, 5 * kHour, 6 * kHour};
	if (run.audio_steps || run.video_steps) {
		for (const double e : edges) {
			if (c >= e && c < e + 120.0)
				return false;
		}
	}
	if (run.receive_gap && c >= 7 * kHour && c < 7 * kHour + 3.0 + 120.0)
		return false;
	return true;
}

void note(PathResult &r, bool &has_prev, double &prev, double av, bool resynced, double c, const LongRun &run)
{
	if (c >= 60.0 && !r.start)
		r.start = av;
	if (has_prev && !resynced)
		r.max_jump_us = std::max(r.max_jump_us, std::fabs(av - prev));
	if (r.start && settled(run, c))
		r.max_dev_us = std::max(r.max_dev_us, std::fabs(av - *r.start));
	r.end_us = av;
	prev = av;
	has_prev = true;
}

// 整列したフレームの、中身の時刻と OBS に出る時刻 (us)。
struct ShownFrame {
	double content_s = 0.0;
	double shown_us = 0.0;
};

LongRunResult run_long(const LongRun &run, std::uint64_t seed)
{
	const double d = run.d_ppm * 1e-6;
	const double la = 0.080;         // 音の上流の遅れ (再生の時刻 - 中身の時刻)
	const double lv = 0.060;         // 映像の上流の遅れ
	const double sender_q = 1.7e9;   // 送信側の時計 - Q (秒。送信側の時計はエポックに近い値から始まる)
	const double js_a = 0.002;       // 送信側の定数の推定に残る、音の届くまでの遅れ
	const double js_v = 0.004;       // 同じく映像
	const double display_d = 0.100;  // 表示の緩衝 D
	const double half_tick = 0.008333;
	const double q0 = 5000.0;        // 模擬を始める Q (秒)
	Rng rng(seed);

	AudioDriftConfig config;
	AudioDriftTracker mix(config);
	AudioDriftTracker rec(config);
	TimestampAligner aligner;
	std::int64_t offset_v_us = 0; // 整列の出力 - 送信側のタイムスタンプ (最新の映像)
	std::int64_t next_frame = 0;
	ShownFrame shown_prev;
	ShownFrame shown_last;
	bool has_shown = false;

	std::optional<std::int64_t> rec_origin_us;
	std::optional<double> mix_origin_us;
	LongRunResult result;
	bool mix_has_prev = false, rec_has_prev = false;
	double mix_prev = 0.0, rec_prev = 0.0;
	double burst_ts_q = 0.0;

	const std::int64_t chunks = std::int64_t(run.hours * kHour * kRate * (1.0 + d) / double(kChunk));
	for (std::int64_t i = 0; i < chunks; ++i) {
		const double c = double(i) * double(kChunk) / (kRate * (1.0 + d));
		const double play_q = q0 + c + la + upstream_audio(run, c);

		// この音より前に再生される映像を、整列に通す (受信の途切れの間は届かない)。
		for (;;) {
			const double cv = double(next_frame) / 60.0;
			const double render_q = q0 + cv + lv + upstream_video(run, cv);
			if (render_q > play_q)
				break;
			++next_frame;
			if (in_gap(run, cv))
				continue;
			const std::int64_t ts_v = std::llround((render_q + sender_q + js_v) * 1e6 + rng.uniform_sym(1000.0));
			const std::int64_t out = aligner.align(ts_v);
			offset_v_us = out - ts_v;
			shown_prev = shown_last;
			shown_last = {cv, double(out) - sender_q * 1e6 + display_d * 1e6};
			has_shown = true;
		}
		if (in_gap(run, c))
			continue;

		// AudioData.timestamp。音の装置が device_chunks 個のチャンクをまとめて取り出すと、まとめた中の後ろのチャンクも
		// 最初のチャンクを取り出した時刻を持つ。
		if (i % run.device_chunks == 0)
			burst_ts_q = play_q + rng.uniform_sym(run.ts_jitter_us) * 1e-6;
		const std::int64_t ts_a = std::llround((burst_ts_q + sender_q + js_a) * 1e6);

		// 録画の経路。映像は送信側のタイムスタンプのまま置くので、同じ中身の時刻の映像の時刻は、揺れを除くと
		// Q の c + Lv + up_v(c) + 送信側の定数 である。
		if (!rec_origin_us)
			rec_origin_us = ts_a;
		{
			const double video_us = (q0 + c + lv + upstream_video(run, c) + js_v) * 1e6;
			const double target = double(ts_a - *rec_origin_us) * kRate / 1e6;
			const double continued = rec.next_input_pos();
			const AudioDriftStep step = rec.push(target, kChunk);
			const double landed = step.resynced ? target : continued;
			const double audio_us = double(*rec_origin_us) + landed * 1e6 / kRate - sender_q * 1e6;
			note(result.rec, rec_has_prev, rec_prev, audio_us - video_us, step.resynced, c, run);
		}

		// 配信の経路。送信側の時計は、接続ごとの差 (sender_q) を引いて OBS の時計 (Q) に換算する。
		if (has_shown) {
			const double at_obs_us = double(ts_a + offset_v_us) - sender_q * 1e6 + (display_d + half_tick) * 1e6;
			if (!mix_origin_us)
				mix_origin_us = at_obs_us - 200000.0;
			const double target = (at_obs_us - *mix_origin_us) * kRate / 1e6;
			// 届いた時刻 (2 ms から 5 ms の揺れ) と、そのときの出し終えた位置 (今 + 先渡しの幅 - 区間の長さ)。
			const double arrival_obs_us = (burst_ts_q + js_a + 0.002) * 1e6 + rng.uniform01() * 3000.0;
			const std::int64_t min_pos = std::int64_t((arrival_obs_us + 45000.0 - *mix_origin_us) * kRate / 1e6);
			const double continued = mix.next_input_pos();
			const AudioDriftStep step = mix.push(target, kChunk, min_pos);
			const double landed = step.resynced ? target : continued;
			const double audio_us = *mix_origin_us + landed * 1e6 / kRate;
			// 同じ中身の時刻の映像が出る時刻。直前の 2 フレームの出た時刻から、中身の時刻で補う。
			double video_us = shown_last.shown_us;
			if (shown_last.content_s > shown_prev.content_s) {
				const double slope = (shown_last.shown_us - shown_prev.shown_us) /
						     (shown_last.content_s - shown_prev.content_s);
				video_us = shown_last.shown_us + (c - shown_last.content_s) * slope;
			}
			note(result.mix, mix_has_prev, mix_prev, audio_us - video_us, step.resynced, c, run);
		}
	}
	result.mix.resyncs = mix.stats().resyncs;
	result.rec.resyncs = rec.stats().resyncs;
	return result;
}

// 終わり - 始め (us)。
double drift_of(const PathResult &p)
{
	return p.start ? p.end_us - *p.start : 0.0;
}

void print_row(const LongRun &run, const LongRunResult &r)
{
	std::printf("| %s | %+.0f | %.1f | %.3f | %.3f | %.3f | %llu | %.3f | %.3f | %.3f | %llu |\n", run.name, run.d_ppm,
		    run.hours, drift_of(r.mix) / 1000.0, r.mix.max_jump_us / 1000.0, r.mix.max_dev_us / 1000.0,
		    static_cast<unsigned long long>(r.mix.resyncs), drift_of(r.rec) / 1000.0, r.rec.max_jump_us / 1000.0,
		    r.rec.max_dev_us / 1000.0, static_cast<unsigned long long>(r.rec.resyncs));
}

void print_header()
{
	std::printf("\n| 模擬 | D-Q (ppm) | 時間 | 配信: 終わり-始め (ms) | 配信: 最大の飛び (ms) | 配信: 最大のずれ (ms) | "
		    "配信: 置き直し | 録画: 終わり-始め (ms) | 録画: 最大の飛び (ms) | 録画: 最大のずれ (ms) | "
		    "録画: 置き直し |\n");
	std::printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
}

void check_clean(const LongRunResult &r)
{
	for (const PathResult *p : {&r.mix, &r.rec}) {
		CHECK(std::fabs(drift_of(*p)) < 2000.0);
		CHECK(p->max_jump_us < 5000.0);
		CHECK(p->max_dev_us < 3000.0);
		CHECK(p->resyncs == 0);
	}
}

} // namespace

TEST_CASE("10 時間の模擬: 音の時計が Q より +-50 と +-100 ppm ずれても、音と映像のずれに時間に比例する成分も 5 ms を超える飛びもない")
{
	const LongRun runs[] = {
		{"D +100 ppm", 100.0},
		{"D +50 ppm", 50.0},
		{"D -50 ppm", -50.0},
		{"D -100 ppm", -100.0},
	};
	print_header();
	std::uint64_t seed = 11;
	for (const LongRun &run : runs) {
		CAPTURE(run.name);
		const LongRunResult r = run_long(run, seed++);
		print_row(run, r);
		check_clean(r);
	}
}

TEST_CASE("10 時間の模擬: 音の装置が 2 チャンクずつ取り出して時刻が 10 ms の幅で揺れても、置き直さずにならす")
{
	LongRun run{"D +100 ppm, 2 チャンクずつ取り出す", 100.0};
	run.device_chunks = 2;
	run.ts_jitter_us = 2000.0;
	const LongRunResult r = run_long(run, 21);
	print_header();
	print_row(run, r);
	check_clean(r);
}

TEST_CASE("10 時間の模擬: 音だけが 15 ms と 8 ms 動く上流の段は、置き直さずにゆっくり追い、5 ms を超えて飛ばない")
{
	LongRun run{"D -50 ppm, 音の上流の段", -50.0};
	run.audio_steps = true;
	const LongRunResult r = run_long(run, 31);
	print_header();
	print_row(run, r);
	check_clean(r);
}

TEST_CASE("10 時間の模擬: 映像だけが 17 ms 動く上流の段は映像の時刻の段として出て、音は飛ばず、戻った後は始めと同じずれになる")
{
	LongRun run{"D +50 ppm, 映像の上流の段", 50.0};
	run.video_steps = true;
	const LongRunResult r = run_long(run, 32);
	print_header();
	print_row(run, r);
	// 映像の 17 ms の段は、60 fps の 1 フレームの間隔とほぼ同じで、整列 (TimestampAligner) はフレームの欠落と区別できず、
	// 進む向きの段を 1 フレーム分の段として出す。録画は映像を送信側のタイムスタンプのまま置くので、段がそのまま映像の
	// 時刻の段になる。どちらも音を置く位置は飛ばないので、ずれの飛びは映像の段 (17 ms) と 5 ms の和を超えない。
	for (const PathResult *p : {&r.mix, &r.rec}) {
		CHECK(p->max_jump_us < 17000.0 + 5000.0);
		CHECK(p->resyncs == 0);
		CHECK(std::fabs(drift_of(*p)) < 2000.0);
		CHECK(p->max_dev_us < 3000.0);
	}
}

TEST_CASE("10 時間の模擬: 3 秒の受信の途切れでは 1 回だけ置き直し、その後は途切れの前と同じずれに戻る")
{
	LongRun run{"D -100 ppm, 3 秒の途切れ", -100.0};
	run.receive_gap = true;
	const LongRunResult r = run_long(run, 41);
	print_header();
	print_row(run, r);
	for (const PathResult *p : {&r.mix, &r.rec}) {
		CHECK(p->resyncs == 1);
		CHECK(p->max_jump_us < 5000.0);
		CHECK(std::fabs(drift_of(*p)) < 2000.0);
		CHECK(p->max_dev_us < 3000.0);
	}
}
