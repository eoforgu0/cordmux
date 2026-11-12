// SPDX-License-Identifier: Apache-2.0

// align/audio_drift.h の変換の計算の 2 つの実装を、同じ入力で比べる。x64 の既定の計算 (detail::AudioDriftDefaultOps) は
// SSE2 の組み込み関数を使い、組み込み関数を使わない予備の計算 (detail::AudioDriftPortableOps) は製品のビルドでは
// 使われないので、ここで必ずコンパイルして確かめる。位置 (入力の整数部と位相) は 1 ビットも違わないことを、値は
// 2 つの差が丸めの誤差の見積もりの上限以内であることを確かめる。2 つの実装は和を同じ順で足すので、MSVC の x64 では
// 値も同じになるが、コンパイラーの設定 (掛け算と足し算を 1 つの命令にまとめるなど) で丸めが変わりうるので、合否は
// 上限で決める。

#include "doctest.h"

#include "align/audio_drift.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <vector>

using cordmux::align::AudioDriftCorrector;
using cordmux::align::AudioDriftStats;
using cordmux::align::AudioDriftStep;
using cordmux::align::BasicAudioDriftCorrector;
namespace detail = cordmux::align::detail;

namespace {

using PortableCorrector = BasicAudioDriftCorrector<detail::AudioDriftPortableOps>;

// 位相の間を直線で補った係数の絶対値の和の、全部の位相での最大。補った係数の絶対値は、両端の位相の係数の絶対値の
// 大きいほうを (丸めの分を除いて) 超えないので、タップごとにその大きいほうを足す。
double max_coef_abs_sum()
{
	const float *table = detail::audio_drift_kernel();
	double best = 0.0;
	for (int p = 0; p < detail::kAudioDriftPhases; ++p) {
		const float *row = table + std::size_t(p) * detail::kAudioDriftRow;
		double sum = 0.0;
		for (int k = 0; k < detail::kAudioDriftTaps; ++k) {
			const double at0 = std::fabs(double(row[k]));
			const double at1 = std::fabs(double(row[k]) + double(row[detail::kAudioDriftTaps + k]));
			sum += std::max(at0, at1);
		}
		best = std::max(best, sum);
	}
	return best;
}

// 32 個の積の和を、2 つの違う順で足したときの差の上限 (入力の絶対値の最大が 1 のとき)。1 つの順の丸めの誤差は
// 31 x 2^-24 x (積の絶対値の和) 以下なので、2 つの差はその 2 倍以下。係数の丸めの違いの分として 1 % を足す。
double sum_order_bound()
{
	return 2.0 * 31.0 * std::ldexp(1.0, -24) * max_coef_abs_sum() * 1.01;
}

struct OpsCase {
	const char *name;
	std::size_t channels;
	double d_ppm;      // 音の時計の速さの差 (目標の進みを chunk / (1 + d) にする)
	double jitter_us;  // 目標の時刻の揺れ (一様、+-)
	double offset;     // 目標の位置の初めの値 (端数があると、比が 1 でも変換する)
	int chunk_mode;    // 0: 480 固定、1: 441、480、960 を順に、2: 1 から 1024 の乱数
	int jump_every;    // この数のチャンクごとに、目標を +-100 ms 飛ばす (置き直し)
	int reset_every;   // この数のチャンクごとに reset する
	int min_pos_every; // この数のチャンクごとに、書き出してよい位置を続きより後ろにする (置き直し)
	float amplitude;   // 入力 (一様な雑音) の絶対値の最大
	int chunks;
};

struct OpsResult {
	std::uint64_t step_mismatch = 0; // 位置、数、置き直し、目標との差のどれかが違った push の数
	std::uint64_t samples = 0;
	std::uint64_t resyncs = 0;
	double max_diff = 0.0;
	bool stats_equal = false;
};

bool same_stats(const AudioDriftStats &a, const AudioDriftStats &b)
{
	return a.resyncs == b.resyncs && a.drift_ppm == b.drift_ppm && a.rate_ppm == b.rate_ppm &&
	       a.max_abs_error_us == b.max_abs_error_us;
}

OpsResult run_both(const OpsCase &c)
{
	OpsResult result;
	AudioDriftCorrector fast({}, c.channels);
	PortableCorrector portable({}, c.channels);
	std::mt19937 rng(20261113);
	std::uniform_real_distribution<float> noise(-c.amplitude, c.amplitude);
	std::uniform_real_distribution<double> jitter(-1.0, 1.0);
	std::uniform_int_distribution<int> random_frames(1, 1024);
	const int fixed_frames[] = {441, 480, 960};
	std::vector<float> in;
	std::vector<float> out_fast;
	std::vector<float> out_portable;
	double content = 0.0; // 受け取った入力のサンプルの数
	double offset = c.offset;
	for (int i = 0; i < c.chunks; ++i) {
		std::size_t frames = 480;
		if (c.chunk_mode == 1)
			frames = std::size_t(fixed_frames[i % 3]);
		else if (c.chunk_mode == 2)
			frames = std::size_t(random_frames(rng));
		in.resize(frames * c.channels);
		for (float &x : in)
			x = noise(rng);
		if (c.jump_every > 0 && i > 0 && i % c.jump_every == 0)
			offset += (i / c.jump_every) % 2 ? 4800.0 : -4800.0;
		if (c.reset_every > 0 && i > 0 && i % c.reset_every == 0) {
			fast.reset();
			portable.reset();
		}
		double target = content / (1.0 + c.d_ppm * 1e-6) + offset + jitter(rng) * c.jitter_us * 0.048;
		std::optional<std::int64_t> min_pos;
		if (c.min_pos_every > 0 && i > 0 && i % c.min_pos_every == 0) {
			const double next = fast.tracker().next_input_pos();
			target = next + 1440.0;
			min_pos = std::int64_t(next) + 2000;
			offset += 1440.0;
		}
		const AudioDriftStep a = fast.push(target, in.data(), frames, &out_fast, min_pos);
		const AudioDriftStep b = portable.push(target, in.data(), frames, &out_portable, min_pos);
		content += double(frames);
		if (a.resynced != b.resynced || a.start_pos != b.start_pos || a.frames != b.frames ||
		    a.error_us != b.error_us || out_fast.size() != out_portable.size()) {
			++result.step_mismatch;
			continue;
		}
		for (std::size_t k = 0; k < out_fast.size(); ++k)
			result.max_diff =
				std::max(result.max_diff, std::fabs(double(out_fast[k]) - double(out_portable[k])));
		result.samples += out_fast.size();
	}
	result.stats_equal = same_stats(fast.stats(), portable.stats());
	result.resyncs = fast.stats().resyncs;
	return result;
}

} // namespace

TEST_CASE(
	"2 つの実装の位置は、端の値 (端数がとても小さい、比がちょうど 1、位置が負、番号が大きい) でも 1 ビットも違わない")
{
	std::vector<double> f0s = {0.0, 1e-300, 1e-17, 2.3e-16, 1e-12, 0.5, 0.999999999999, std::nextafter(1.0, 0.0)};
	std::vector<double> incs = {1.0,
				    1.0 / (1.0 + 1e-3),
				    1.0 / (1.0 - 1e-3),
				    1.0 / (1.0 + 300e-6),
				    std::nextafter(1.0, 2.0),
				    std::nextafter(1.0, 0.0),
				    -0.5,
				    1.0 / 0.95};
	std::mt19937 rng(7);
	std::uniform_real_distribution<double> unit(0.0, 1.0);
	std::uniform_real_distribution<double> ppm(-1000.0, 1000.0);
	for (int i = 0; i < 40; ++i) {
		f0s.push_back(unit(rng));
		incs.push_back(1.0 / (1.0 + ppm(rng) * 1e-6));
	}
	// 2000000000 は、2 つずつ求める実装が整数部を 32 ビットで扱えず、1 つずつ求める所。
	const std::size_t j0s[] = {0, 64, 448, 1000000, 300000000, 2000000000};
	std::uint64_t checked = 0;
	std::uint64_t mismatched = 0;
	for (const double f0 : f0s) {
		for (const double inc : incs) {
			for (const std::size_t j0 : j0s) {
				// n は奇数 (2 つずつ求める実装は、最後の組の 2 つ目も書く。その分の長さを配列に持たせる)。
				constexpr std::size_t n = 63;
				std::int64_t first_a[n + 1];
				int phase_a[n + 1];
				float frac_a[n + 1];
				std::int64_t first_b[n + 1];
				int phase_b[n + 1];
				float frac_b[n + 1];
				detail::AudioDriftDefaultOps::positions(f0, inc, -15, j0, n, first_a, phase_a, frac_a);
				detail::AudioDriftPortableOps::positions(f0, inc, -15, j0, n, first_b, phase_b, frac_b);
				for (std::size_t b = 0; b < n; ++b) {
					++checked;
					if (first_a[b] != first_b[b] || phase_a[b] != phase_b[b] ||
					    std::memcmp(&frac_a[b], &frac_b[b], sizeof(float)) != 0)
						++mismatched;
				}
			}
		}
	}
	CHECK(checked > 100000);
	CHECK(mismatched == 0);
}

TEST_CASE("2 つの実装は、同じ入力で同じ位置と数を書き出し、値の差は丸めの誤差の見積もりの上限以内")
{
	const double bound = sum_order_bound();
	// 上限は -100 dB 前後 (係数の絶対値の和は約 2.4)。それより大きいと、上限が誤りを見逃す。
	CHECK(bound < 1e-5);
	const OpsCase cases[] = {
		{"2 チャンネル、+300 ppm、揺れ 1 ms", 2, 300.0, 1000.0, 1000.0, 0, 0, 0, 0, 1.0f, 2000},
		{"2 チャンネル、-1000 ppm (上限)", 2, -1000.0, 0.0, 1000.0, 0, 0, 0, 0, 1.0f, 2000},
		{"2 チャンネル、比が 1 で端数がある", 2, 0.0, 0.0, 1000.37, 0, 0, 0, 0, 1.0f, 500},
		{"2 チャンネル、0 ppm、揺れ 1 ms (変換と変換なしが入れ替わる)", 2, 0.0, 1000.0, 1000.0, 0, 0, 0, 0,
		 1.0f, 2000},
		{"2 チャンネル、チャンク 1 から 1024、+200 ppm、揺れ 3 ms", 2, 200.0, 3000.0, 1000.0, 2, 0, 0, 0, 1.0f,
		 2000},
		{"2 チャンネル、置き直しと reset と書き出してよい位置", 2, 100.0, 1000.0, 1000.0, 1, 500, 700, 300,
		 1.0f, 3000},
		{"2 チャンネル、振幅 8", 2, -300.0, 1000.0, 1000.0, 0, 0, 0, 0, 8.0f, 1000},
		{"1 チャンネル、+300 ppm、置き直しあり", 1, 300.0, 1000.0, 1000.0, 0, 400, 0, 0, 1.0f, 2000},
		{"6 チャンネル、-300 ppm、チャンク 441/480/960、置き直しあり", 6, -300.0, 1000.0, 1000.0, 1, 400, 0, 0,
		 1.0f, 1500},
	};
	for (const OpsCase &c : cases) {
		CAPTURE(c.name);
		const OpsResult r = run_both(c);
		CHECK(r.step_mismatch == 0);
		CHECK(r.stats_equal);
		CHECK(r.samples > 100000);
		CHECK(r.max_diff <= bound * double(c.amplitude));
		if (c.jump_every > 0 || c.min_pos_every > 0)
			CHECK(r.resyncs > 0);
	}
}
