// SPDX-License-Identifier: Apache-2.0

// align/timestamp_aligner.h (TimestampAligner) の単体テスト。
// テストに使うタイムスタンプの列 (fixture) は fixtures.h が作る。

#include "doctest.h"

#include "align/timestamp_aligner.h"
#include "fixtures.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using cordmux::align::AlignerConfig;
using cordmux::align::AlignerStats;
using cordmux::align::TimestampAligner;
using cordmux::tests::Fixture;

namespace {

// 1 フレームごとの出力と統計。テストはここから読む。
struct Trace {
	std::vector<int64_t> out;
	std::vector<double> period;
	std::vector<int64_t> carry;
	std::vector<int64_t> step_frames;
	std::vector<double> slew;
	AlignerStats final_stats;
};

Trace run(const Fixture &f, const AlignerConfig &cfg = AlignerConfig{})
{
	TimestampAligner a(cfg);
	Trace t;
	const size_t n = f.ts_us.size();
	t.out.reserve(n);
	t.period.reserve(n);
	t.carry.reserve(n);
	t.step_frames.reserve(n);
	t.slew.reserve(n);
	for (int64_t ts : f.ts_us) {
		t.out.push_back(a.align(ts));
		const AlignerStats &s = a.stats();
		t.period.push_back(s.period_us);
		t.carry.push_back(s.carry_us);
		t.step_frames.push_back(s.last_step_frames);
		t.slew.push_back(s.last_slew_us);
	}
	t.final_stats = a.stats();
	return t;
}

// 定常とみなすフレームか (最初の 600 フレーム、不連続のフレーム、整定中のフレームを除く)。
bool is_steady(const Trace &t, size_t n, size_t warmup = 600)
{
	if (n < warmup || n == 0)
		return false;
	if (t.step_frames[n] != 1)
		return false;
	if (t.carry[n] != 0 || t.carry[n - 1] != 0)
		return false;
	return true;
}

const double kSlotUs = 1000000.0 / 60.0; // OBS の 60 Hz の描画の 1 周期

struct SlotResult {
	int64_t slots = 0;
	int64_t empty = 0;  // フレームが 1 つもない周期 = 重複 (前のフレームがもう一度表示される)
	int64_t doubled = 0; // フレームが 2 つ以上ある周期 = 欠落 (1 つを残して捨てられる)
	double rate = 0.0;
};

// 出力を OBS の 60 Hz の描画の周期に割り当て、フレームが 0 個の周期と 2 個以上の周期を数える。
// 周期の位相は、最初のフレームが周期の中央に来るように置く (OBS の描画の位相は送信側と関係がなく、
// フレームが周期の境目にちょうど乗る位相は確率 0 なので、中央に置いて調べる)。
SlotResult slot_simulate(const Trace &t, size_t from)
{
	SlotResult r;
	if (from >= t.out.size())
		return r;
	const double base = double(t.out[from]) - kSlotUs / 2.0;
	std::map<int64_t, int64_t> hits;
	for (size_t n = from; n < t.out.size(); n++) {
		const int64_t slot = int64_t(std::floor((double(t.out[n]) - base) / kSlotUs));
		hits[slot]++;
	}
	const int64_t first = hits.begin()->first;
	const int64_t last = hits.rbegin()->first;
	r.slots = last - first + 1;
	r.empty = r.slots - int64_t(hits.size());
	for (const auto &kv : hits)
		if (kv.second >= 2)
			r.doubled++;
	r.rate = r.slots > 0 ? double(r.empty + r.doubled) / double(r.slots) : 0.0;
	return r;
}

// jump_excuse_max_us と max_slew_ratio を 0 にした形。2 つの上限が動作していることを、既定の形と比べて
// 確認するのに使う。
AlignerConfig before_plan_c()
{
	AlignerConfig cfg;
	cfg.jump_excuse_max_us = 0.0; // 平滑化した誤差を全部差し引く
	cfg.max_slew_ratio = 0.0;     // 補正の上限は 1 フレームあたり 500 us だけ
	return cfg;
}

// 出力のずれ (出力 - 送信側) が一方向にたまっていく速さ。単位は無次元 (ずれの us / 経過時間の us)。
//
// 最初の 1 分 (立ち上がり) を除き、送信側の経過時間で 10 分ごとに区切ってずれの平均を取り、その平均を
// 区間の中央の時刻へ最小二乗で当てはめた傾きを返す。区間ごとに平均するのは、フレームの多い区間
// (フレームレートの高い区間) に引っ張られず、時間で等しく重みを付けるためである。間隔が不規則な列では
// 1 フレームごとのずれが数百 ms から数秒の幅でばらつくので、最後のフレームのずれ / 経過時間 で測ると、
// そのばらつきがそのままたまった量に見えてしまう。
double drift_slope(const Fixture &f, const Trace &t)
{
	const double skip_us = 60e6;
	const double bin_us = 600e6;
	const int64_t t0 = f.ts_us.front();
	std::vector<double> sum;
	std::vector<double> cnt;
	for (size_t n = 0; n < f.ts_us.size(); n++) {
		const double el = double(f.ts_us[n] - t0);
		if (el < skip_us)
			continue;
		const size_t b = size_t((el - skip_us) / bin_us);
		if (b >= sum.size()) {
			sum.resize(b + 1, 0.0);
			cnt.resize(b + 1, 0.0);
		}
		sum[b] += double(t.out[n] - f.ts_us[n]);
		cnt[b] += 1.0;
	}
	double w = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
	for (size_t b = 0; b < sum.size(); b++) {
		if (cnt[b] <= 0.0)
			continue;
		const double x = skip_us + (double(b) + 0.5) * bin_us;
		const double y = sum[b] / cnt[b];
		w += 1.0;
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
	}
	const double denom = w * sxx - sx * sx;
	return denom > 0.0 ? (w * sxy - sx * sy) / denom : 0.0;
}

// |出力 - 送信側| の最大 (us)。
int64_t max_abs_offset(const Fixture &f, const Trace &t, size_t from = 0)
{
	int64_t worst = 0;
	for (size_t n = from; n < f.ts_us.size(); n++)
		worst = std::max(worst, std::llabs(t.out[n] - f.ts_us[n]));
	return worst;
}

// |出力 - 送信側| の分位点 (us)。最初の 1 分 (立ち上がり) を除く。
double abs_offset_quantile(const Fixture &f, const Trace &t, double q)
{
	std::vector<int64_t> v;
	for (size_t n = 0; n < f.ts_us.size(); n++)
		if (double(f.ts_us[n] - f.ts_us.front()) >= 60e6)
			v.push_back(std::llabs(t.out[n] - f.ts_us[n]));
	if (v.empty())
		return 0.0;
	std::sort(v.begin(), v.end());
	return double(v[size_t(q * double(v.size() - 1))]);
}

// 補正が上限で頭打ちになったフレームの割合。
double slew_capped_ratio(const Trace &t)
{
	return t.final_stats.frames > 0
	               ? double(t.final_stats.slew_capped_frames) / double(t.final_stats.frames)
	               : 0.0;
}

// 一方向のずれの傾き (drift_slope) の上限。
// m4 (まばらな列) だけ 0.05% に緩める。W を超えるずれが最大 7-10 秒あり、10 分ごとの平均にもそのずれが
// 残るので、120 分の列でも、傾きが乱数のシードによって +-0.0125% まで振れた (シード 8 通りで測った)。
// 2 つの上限を 0 にした形では 3.2% なので、0.05% でも、ずれがたまる形とは 60 倍離れている。
double drift_limit(const Fixture &f)
{
	return f.name.rfind("m4", 0) == 0 ? 0.0005 : 0.0001;
}

// Firefox から画面共有した映像を模した、間隔が不規則な列 (m1 と m2) か。|ずれ| の最大と、補正が
// 頭打ちになったフレームの割合を確認する列である。
bool is_firefox_like(const Fixture &f)
{
	return f.name.rfind("m1", 0) == 0 || f.name.rfind("m2", 0) == 0;
}

} // namespace

TEST_CASE("どの fixture でも出力は狭義単調増加である")
{
	for (const Fixture &f : cordmux::tests::all_fixtures()) {
		CAPTURE(f.name);
		const Trace t = run(f);
		bool ok = true;
		size_t bad = 0;
		for (size_t n = 1; n < t.out.size(); n++) {
			if (t.out[n] <= t.out[n - 1]) {
				ok = false;
				bad = n;
				break;
			}
		}
		CAPTURE(bad);
		CHECK(ok);
	}
}

TEST_CASE("定常では出力の差分が推定周期の +-50 us に収まる")
{
	for (const Fixture &f : cordmux::tests::all_fixtures()) {
		CAPTURE(f.name);
		const Trace t = run(f);
		double worst = 0.0;
		size_t worst_at = 0;
		int64_t checked = 0;
		for (size_t n = 1; n < t.out.size(); n++) {
			if (!is_steady(t, n))
				continue;
			checked++;
			const double d = std::fabs(double(t.out[n] - t.out[n - 1]) - t.period[n]);
			if (d > worst) {
				worst = d;
				worst_at = n;
			}
		}
		CAPTURE(checked);
		CAPTURE(worst_at);
		CAPTURE(worst);
		CHECK(checked > 500); // 数えるフレームがないまま成功しないようにする
		CHECK(worst <= 50.0);
	}
}

TEST_CASE("定常では |送信側 - 出力 - 平均オフセット| が 1 周期より小さい")
{
	for (const Fixture &f : cordmux::tests::fixed_period_fixtures()) {
		CAPTURE(f.name);
		const Trace t = run(f);
		std::vector<double> offs;
		for (size_t n = 600; n < t.out.size(); n++) {
			// 不連続の 1 つ前のフレームは外す。不連続を次のフレームで確認する間は、そのフレームを
			// グリッドの次の位置 (+1 周期) に置いて先に返す (出力の遅延を足さない) ので、実際の
			// ドロップだったときは、そのフレームだけ送信側より 1 周期遅れる。次のフレームで追いつく
			// 一時的なもので、定常のオフセットではない。
			if (n + 1 < t.step_frames.size() && t.step_frames[n + 1] > 1)
				continue;
			offs.push_back(double(f.ts_us[n] - t.out[n]));
		}
		REQUIRE(offs.size() > 500);
		double sum = 0.0;
		for (double o : offs)
			sum += o;
		const double mean = sum / double(offs.size());
		double worst = 0.0;
		for (double o : offs)
			worst = std::max(worst, std::fabs(o - mean));
		CAPTURE(mean);
		CAPTURE(worst);
		CHECK(worst < f.true_period_us);
	}
}

TEST_CASE("ジッターだけの fixture では再整列も再ロックも不連続も起きない")
{
	for (const Fixture &f : cordmux::tests::jitter_only_fixtures()) {
		CAPTURE(f.name);
		const Trace t = run(f);
		CHECK(t.final_stats.realign_count == 0);
		CHECK(t.final_stats.relock_count == 0);
		CHECK(t.final_stats.jump_count == 0);
	}
}

TEST_CASE("フレームレートが 30、60、30 fps と変わる fixture では、再ロックが 2 回起き、再整列は起きない")
{
	const Fixture f = cordmux::tests::fixture_e();
	const Trace t = run(f);
	CAPTURE(t.final_stats.relock_count);
	CAPTURE(t.final_stats.realign_count);
	CHECK(t.final_stats.relock_count == 2);
	CHECK(t.final_stats.realign_count == 0);

	// 変化の後、それぞれの区間の終わりでは新しい周期になっている。
	CHECK(std::fabs(t.period[1199] - cordmux::tests::period_60000()) <
	      cordmux::tests::period_60000() * 0.01);
	CHECK(std::fabs(t.period[1799] - cordmux::tests::period_30()) <
	      cordmux::tests::period_30() * 0.01);
}

// 送信側のフレームレートが持続的に変わったら、新しい周期へ収束して定常に戻ることを確認する。
// 次の 2 つの誤りを防ぐ。
//   (a) 不連続のフレームで回帰の原点をグリッドのフレーム数 k で動かすと、変化が relock_deviation (20%)
//       未満のときに推定周期が変化前の値に張り付き、不連続が出続ける (timestamp_aligner.cpp の
//       「回帰の原点を動かすフレーム数」のコメント)。
//   (b) 差分 1 つずつに 20% を課す再ロックの判定では、20% の変化を検出できない
//       (AlignerConfig::relock_frames のコメント)。
//
// 判定は、最後に定常へ戻っているかで見る。持続的な変化の間は補正が働くので、「出力の差分が推定周期の
// +-50 us に収まる」は途中では成り立たない。
TEST_CASE("フレームレートが持続的に変わると、新しい周期に収束して定常に戻る")
{
	const int phase1 = 3000;
	const int phase2 = 12000;
	const size_t tail = 2000; // 最後の 2000 フレームは定常であること
	for (double dev : cordmux::tests::sustained_devs()) {
		for (uint64_t seed : {601500ull, 8123ull, 99991ull}) {
			const Fixture f = cordmux::tests::fixture_i(dev, phase1, phase2, seed);
			CAPTURE(f.name);
			CAPTURE(seed);
			const Trace t = run(f);
			CAPTURE(t.final_stats.relock_count);
			CAPTURE(t.final_stats.realign_count);
			CAPTURE(t.final_stats.jump_count);
			CAPTURE(t.final_stats.period_us);
			CAPTURE(f.true_period_us);

			// 出力は常に狭義単調増加。
			for (size_t n = 1; n < t.out.size(); n++)
				REQUIRE(t.out[n] > t.out[n - 1]);

			// 変化後の周期へ 0.5% 以内で収束している (測った最悪の値は 4860 フレーム = 81 秒。
			// 変化の割合 12 通りと乱数のシード 10 通りで測った)。
			const double err = std::fabs(t.final_stats.period_us - f.true_period_us) /
			                   f.true_period_us;
			CAPTURE(err);
			CHECK(err <= 0.005);

			// 最後の tail フレームは定常で、グリッドを余分に進めず、出力の差分が推定周期に収まる。
			const size_t from = t.out.size() - tail;
			int64_t extra_steps = 0;
			double worst_step = 0.0;
			for (size_t n = from; n < t.out.size(); n++) {
				if (t.step_frames[n] != 1)
					extra_steps++;
				const double d = std::fabs(double(t.out[n] - t.out[n - 1]) - t.period[n]);
				if (d > worst_step)
					worst_step = d;
			}
			CAPTURE(extra_steps);
			CAPTURE(worst_step);
			CHECK(extra_steps == 0);
			CHECK(worst_step <= 50.0);

			// 20% (relock_deviation) 以上の変化では再ロックする ((b))。
			// 20% 未満でも、補正の間に推定周期が動いて 20% をまたぐと再ロックすることがあるので、
			// 再ロックしないことは求めない。回帰だけで追従できることは、次のテストで確認する。
			if (std::fabs(dev) >= 0.20)
				CHECK(t.final_stats.relock_count >= 1);
		}
	}
}

// 再ロックを使わなくても、20% 未満の持続的な変化には、回帰だけで新しい周期へ追従することを確認する。
// 不連続のフレームで回帰の原点をグリッドのフレーム数 k で動かすと、「数フレームに 1 回 k = 2 で進める」
// 状態が不動点になり、再ロックを使わない限り、推定周期が変化前の値から動かなくなる。
TEST_CASE("再ロックを使わなくても、20% 未満の持続的な変化には回帰が追従する")
{
	AlignerConfig no_relock;
	no_relock.relock_deviation = 10.0; // 実質的に無効にする (1000% ずれたときだけ再ロックする)
	// この値は、再ロックしそうな間に再整列を止める条件 (relock_pending) も外す。変化の割合が負の側
	// (フレームレートが上がり、グリッドが遅れる側) では再整列が 1 回起きるが、そのときの初期値は
	// 変化前の周期なので、最後の周期が目標の 0.5% 以内に入れば、回帰が追従したと言える。
	for (double dev : {0.02, 0.05, 0.10, 0.15, -0.10, -0.15}) {
		for (uint64_t seed : {601500ull, 8123ull, 99991ull}) {
			const Fixture f = cordmux::tests::fixture_i(dev, 3000, 12000, seed);
			CAPTURE(f.name);
			CAPTURE(seed);
			const Trace t = run(f, no_relock);
			CAPTURE(t.final_stats.relock_count);
			CAPTURE(t.final_stats.realign_count);
			CAPTURE(t.final_stats.jump_count);
			CAPTURE(t.final_stats.period_us);
			CAPTURE(f.true_period_us);
			REQUIRE(t.final_stats.relock_count == 0);
			const double err = std::fabs(t.final_stats.period_us - f.true_period_us) /
			                   f.true_period_us;
			CAPTURE(err);
			CHECK(err <= 0.005);
		}
	}
}

// 送信側の「遅れて届いたフレームの次が早く届く」単発のジッターを、不連続と誤判定しないことを確認する
// (fixture j)。グリッドからの残差が余白 (0.5 周期 = 8.3 ms) を超えたフレームを、その場で 1 フレームの
// ドロップと確定すると、送信側が 108000 フレームを 1 つも落とさずに送った列でも 117-119 回を不連続と数え、
// グリッドを余分に進めてから補正で戻す (補正の累計 1.1 秒)。描画では、これが 1 フレームの欠落 109 件と
// 重複 88 件 (各 0.1%) になる (AlignerConfig::jump_confirm_frames のコメント)。
TEST_CASE("遅れて届いたフレームの次が早く届くジッターを、不連続と誤判定しない")
{
	for (uint64_t seed : cordmux::tests::window_seeds()) {
		const Fixture f = cordmux::tests::fixture_j(108000, seed);
		CAPTURE(f.name);
		CAPTURE(seed);

		// 送信側のタイムスタンプは単調増加である (ジッターの作り方の前提。崩れていたら別のテストになる)。
		for (size_t n = 1; n < f.ts_us.size(); n++)
			REQUIRE(f.ts_us[n] > f.ts_us[n - 1]);

		const Trace t = run(f);
		CAPTURE(t.final_stats.jump_pending_count);
		CAPTURE(t.final_stats.jump_wobble_count);
		CAPTURE(t.final_stats.jump_count);
		CAPTURE(t.final_stats.realign_count);
		CAPTURE(t.final_stats.relock_count);
		CAPTURE(t.final_stats.slew_abs_sum_us);

		// 不連続を疑う場面がある (なければ、この fixture はこのジッターを再現していない)。
		CHECK(t.final_stats.jump_pending_count > 100);
		// 疑った場面は、全部ジッターだったと分かる。
		CHECK(t.final_stats.jump_count == 0);
		CHECK(t.final_stats.jump_frames == 0);
		CHECK(t.final_stats.realign_count == 0);
		CHECK(t.final_stats.relock_count == 0);

		// 定常の出力の差分が、推定周期の +-50 us に収まる。
		double worst = 0.0;
		size_t worst_at = 0;
		int64_t checked = 0;
		for (size_t n = 1; n < t.out.size(); n++) {
			if (!is_steady(t, n))
				continue;
			checked++;
			const double d = std::fabs(double(t.out[n] - t.out[n - 1]) - t.period[n]);
			if (d > worst) {
				worst = d;
				worst_at = n;
			}
		}
		CAPTURE(checked);
		CAPTURE(worst_at);
		CAPTURE(worst);
		CHECK(checked > 100000);
		CHECK(worst <= 50.0);

		// 60 Hz の描画に割り当てて、重複も欠落も 0 (送信側は 1 フレームも落としていないので、
		// 整列が作らなければ 0 になる)。
		const SlotResult r = slot_simulate(t, 600);
		CAPTURE(r.slots);
		CAPTURE(r.empty);
		CAPTURE(r.doubled);
		CHECK(r.empty == 0);
		CHECK(r.doubled == 0);
	}

	// 確認する手順を無効にする (jump_confirm_frames = 0) と、同じ列で不連続の誤判定と、描画の欠落と
	// 重複が出る。この手順が動作していることを確認する。
	{
		AlignerConfig no_confirm;
		no_confirm.jump_confirm_frames = 0;
		const Fixture f = cordmux::tests::fixture_j(108000, cordmux::tests::window_seeds()[0]);
		const Trace t = run(f, no_confirm);
		const SlotResult r = slot_simulate(t, 600);
		CAPTURE(t.final_stats.jump_count);
		CAPTURE(t.final_stats.slew_abs_sum_us);
		CAPTURE(r.empty);
		CAPTURE(r.doubled);
		// 送信側は 1 フレームも落としていないのに不連続を数え、描画で欠落と重複を作る。
		CHECK(t.final_stats.jump_count > 100);
		CHECK(r.empty > 50);
		CHECK(r.doubled > 50);
	}
}

// ジッターを見送るようにしても、実際の 1 フレームのドロップは全部不連続として拾うことを確認する
// (fixture k)。見送りすぎると、1 周期ぶんの誤差を補正で戻すことになり、そこで重複が出る。
TEST_CASE("実際の 1 フレームのドロップは全部不連続として数え、余分な欠落や重複を作らない")
{
	for (uint64_t seed : cordmux::tests::window_seeds()) {
		const Fixture f = cordmux::tests::fixture_k(108000, seed);
		CAPTURE(f.name);
		CAPTURE(seed);
		CAPTURE(f.dropped_frames);
		REQUIRE(f.dropped_frames > 1000);
		for (size_t n = 1; n < f.ts_us.size(); n++)
			REQUIRE(f.ts_us[n] > f.ts_us[n - 1]);

		const Trace t = run(f);
		CAPTURE(t.final_stats.jump_pending_count);
		CAPTURE(t.final_stats.jump_wobble_count);
		CAPTURE(t.final_stats.jump_count);
		CAPTURE(t.final_stats.jump_frames);
		CAPTURE(t.final_stats.realign_count);
		CAPTURE(t.final_stats.relock_count);

		// どの不連続もグリッドを 1 周期だけ余分に進めた (1 フレームのドロップなので k = 2)。
		CHECK(t.final_stats.jump_frames == t.final_stats.jump_count);
		CHECK(t.final_stats.realign_count == 0);
		CHECK(t.final_stats.relock_count == 0);

		// 推定周期は真の値の周りを +-0.13% で揺らぐ。不連続のフレームでは回帰の原点を
		// diff / 推定周期 で動かすので、diff に乗るジッターのために、ドロップのたびに回帰の x の側へ
		// ジッター / 周期 = +-0.6 フレームの誤差が入るためである。揺らぐだけで偏りではないことを、
		// 平均で確認する。偏ると、グリッドが送信側から離れ続け、補正が上限で頭打ちになって、描画で
		// 重複が出る。
		double period_sum = 0.0;
		int64_t period_n = 0;
		for (size_t n = 600; n < t.period.size(); n++) {
			period_sum += t.period[n];
			period_n++;
		}
		const double period_mean_err =
		        std::fabs(period_sum / double(period_n) - f.true_period_us) / f.true_period_us;
		CAPTURE(period_sum / double(period_n));
		CAPTURE(period_mean_err);
		CHECK(period_mean_err <= 0.0002);

		// 不連続の数はドロップの数とほぼ一致する。立ち上がりの 600 フレームだけは数件ずれる。
		// 周期の初期値が最初の差分 (ジッター +-5 ms がそのまま乗る) なので、数 ms の誤差を補正で
		// 戻す 200 フレームほどの間、グリッドの予想位置に 1-2 ms の偏りが残り、ジッターだけで
		// 残差が余白を超えることがあるためである (乱数のシード 31337 では、立ち上がりに誤判定 2 回と
		// 見逃し 1 回があり、どちらも描画で欠落も重複も作らなかった)。
		// 定常 (600 フレーム以降) の一致は、下で厳密に確認する。
		CHECK(t.final_stats.jump_count >= f.dropped_frames - 2);
		CHECK(t.final_stats.jump_count <= f.dropped_frames + 2);

		// 不連続のフレームでは、グリッドの間隔が k * 推定周期 に保たれる。
		// 立ち上がりの 600 フレームは、最初の差分を初期値にしたぶんの誤差をまだ補正で戻している
		// 途中で、補正が間隔に乗るので、ほかのテストと同じく外す。
		double worst = 0.0;
		size_t worst_at = 0;
		int64_t jumps = 0;
		for (size_t n = 600; n < t.out.size(); n++) {
			if (t.step_frames[n] <= 1)
				continue;
			jumps++;
			const double expect = t.period[n] * double(t.step_frames[n]);
			const double d = std::fabs(double(t.out[n] - t.out[n - 1]) - expect);
			if (d > worst) {
				worst = d;
				worst_at = n;
			}
		}
		int64_t drops_after_600 = 0;
		for (size_t at : f.after_drop)
			if (at >= 600)
				drops_after_600++;
		CAPTURE(jumps);
		CAPTURE(drops_after_600);
		CAPTURE(worst_at);
		CAPTURE(worst);
		CHECK(jumps == drops_after_600);
		CHECK(worst <= 50.0);

		// 60 Hz の描画で、フレームのない周期の数はドロップの数と同じで、2 フレーム入る周期はない。
		int64_t drops_in_range = 0;
		for (size_t at : f.after_drop)
			if (at >= 600)
				drops_in_range++;
		const SlotResult r = slot_simulate(t, 600);
		CAPTURE(r.slots);
		CAPTURE(r.empty);
		CAPTURE(r.doubled);
		CAPTURE(drops_in_range);
		CHECK(r.empty == drops_in_range);
		CHECK(r.doubled == 0);
	}
}

// ジッターと 2 フレーム連続のドロップが重なっても、再ロックせず、不連続を全部数えることを確認する
// (fixture l)。2 フレーム連続のドロップの差分は 3 周期なので、直近 8 フレームの差分に 1 つ入るだけで平均が
// 0.25 周期上がり、20% のしきい値を超える。外れ値を除かないと、残りの 7 フレームの過半数が、ジッターだけで
// 5% 前後の確率で同じ向きにずれ、ドロップ 1113 回のうち 58 回で誤って再ロックする。そのたびに周期が
// 26-35% 高い値に置き直され、保留中の確認も捨てられるので、不連続の数え漏れと、描画で捨てられるフレームが
// 出る (AlignerConfig::relock_trim のコメント)。
TEST_CASE("ジッターと 2 フレーム連続のドロップが重なっても、再ロックしない")
{
	for (uint64_t seed : cordmux::tests::window_seeds()) {
		const Fixture f = cordmux::tests::fixture_l(108000, seed);
		CAPTURE(f.name);
		CAPTURE(seed);
		CAPTURE(f.dropped_frames);
		REQUIRE(f.dropped_frames > 1000);
		for (size_t n = 1; n < f.ts_us.size(); n++)
			REQUIRE(f.ts_us[n] > f.ts_us[n - 1]);

		const Trace t = run(f);
		CAPTURE(t.final_stats.jump_pending_count);
		CAPTURE(t.final_stats.jump_wobble_count);
		CAPTURE(t.final_stats.jump_count);
		CAPTURE(t.final_stats.jump_frames);
		CAPTURE(t.final_stats.realign_count);
		CAPTURE(t.final_stats.relock_count);

		// 出力は常に狭義単調増加である。
		for (size_t n = 1; n < t.out.size(); n++)
			REQUIRE(t.out[n] > t.out[n - 1]);

		// 送信側のフレームレートは変わっていないので、再ロックも再整列も起きない。
		CHECK(t.final_stats.relock_count == 0);
		CHECK(t.final_stats.realign_count == 0);

		// 不連続で進めたグリッドのフレーム数は、ドロップの数と一致する (立ち上がりの 600 フレームで
		// 数件ずれる理由は、1 フレームのドロップのテストと同じ)。
		CHECK(t.final_stats.jump_frames >= f.dropped_frames - 2);
		CHECK(t.final_stats.jump_frames <= f.dropped_frames + 2);

		// 送信側と出力のオフセットは、際限なく積み上がらない。
		auto steady_offset_mean = [&](size_t from, size_t to) {
			double sum = 0.0;
			int64_t n_ok = 0;
			for (size_t n = from; n < to; n++) {
				if (!is_steady(t, n))
					continue;
				sum += double(f.ts_us[n] - t.out[n]);
				n_ok++;
			}
			REQUIRE(n_ok > 100);
			return sum / double(n_ok);
		};
		const size_t half = t.out.size() / 2;
		const double mean_first = steady_offset_mean(600, half);
		const double mean_last = steady_offset_mean(half, t.out.size());
		CAPTURE(mean_first);
		CAPTURE(mean_last);
		const double drift = mean_last - mean_first;
		CAPTURE(drift);
		CHECK(std::fabs(drift) < f.true_period_us);

		// 60 Hz の描画で、フレームのない周期は実際のドロップのぶんだけある (2 フレーム連続なので、
		// 1 回のドロップで 2 周期)。2 フレーム入る周期 (1 フレームが捨てられる) は、107387 周期で
		// 1 件だけ残る。これは誤判定ではない。ドロップとジッターが重なる列では推定周期が真の値の周りを
		// +-0.13% で揺らぐので、出力と描画の位相がゆっくりずれて、30 分に 1 回境目をまたぐ。
		// 再ロックを無効にしても同じ 1 件が出るので、再ロックとは関係がない。
		const SlotResult r = slot_simulate(t, 600);
		CAPTURE(r.slots);
		CAPTURE(r.empty);
		CAPTURE(r.doubled);
		CAPTURE(r.rate);
		CHECK(r.empty >= f.dropped_frames - 30);
		CHECK(r.empty <= f.dropped_frames + 30);
		CHECK(r.doubled <= 1); // 測るとシード 3 通りのすべてでちょうど 1 件
	}

	// 外れ値を除く手順を無効にする (relock_trim = 0) と、同じ列で誤って再ロックし、不連続を見逃し、
	// 描画で捨てられるフレームが出る。この手順が動作していることを確認する。
	{
		AlignerConfig no_trim;
		no_trim.relock_trim = 0;
		const Fixture f = cordmux::tests::fixture_l();
		const Trace t = run(f, no_trim);
		const SlotResult r = slot_simulate(t, 600);
		CAPTURE(t.final_stats.relock_count);
		CAPTURE(t.final_stats.jump_frames);
		CAPTURE(f.dropped_frames);
		CAPTURE(r.empty);
		CAPTURE(r.doubled);
		CHECK(t.final_stats.relock_count > 20);
		CHECK(t.final_stats.jump_frames < f.dropped_frames);
		CHECK(r.doubled > 20);
	}
}

// 変動フレームレートの列 (fixture m1-m5) で、出力のずれが一方向にたまらないことを確認する。
// 間隔が不規則な入力では、出力が送信側より先に出るずれが時間の 1% 前後の速さでたまる。それを防ぐ
// AlignerConfig::jump_excuse_max_us と max_slew_ratio の効果を、次の条件で確認する。
//   1. 10 分ごとの平均のずれの傾きが、時間の 0.01% 未満 (m4 だけ 0.05%。drift_limit のコメント)
//   2. m1 と m2 で、|ずれ| の最大が 1 秒未満
//   3. m1 と m2 で、補正が上限で頭打ちになったフレームが半分未満
//   4. m5 で、等間隔に戻ってから 60 秒より後の |ずれ| が 10 ms 未満
//   5. 2 つの上限を 0 にした形 (before_plan_c) で同じ列を流すと、1 が失敗する (このテストが問題を
//      見つけられることの確認)
// m3 と m4 では |ずれ| が最大 4-10 秒残る (グリッドは間隔が不規則な列を表せない。TimestampAligner の
// コメント) ので、|ずれ| の最大は条件に入れない。
TEST_CASE("変動フレームレートの列で、出力のずれが一方向にたまらない")
{
	for (const Fixture &f : cordmux::tests::vfr_fixtures()) {
		CAPTURE(f.name);
		for (size_t n = 1; n < f.ts_us.size(); n++)
			REQUIRE(f.ts_us[n] > f.ts_us[n - 1]);

		const Trace t = run(f);
		const double slope = drift_slope(f, t);
		const double capped = slew_capped_ratio(t);
		const int64_t worst = max_abs_offset(f, t);
		CAPTURE(slope);
		CAPTURE(capped);
		CAPTURE(worst);
		CAPTURE(t.final_stats.realign_count);
		CAPTURE(t.final_stats.relock_count);
		CAPTURE(t.final_stats.jump_count);
		CAPTURE(t.final_stats.jump_excuse_capped_frames);

		// 出力は常に狭義単調増加。
		for (size_t n = 1; n < t.out.size(); n++)
			REQUIRE(t.out[n] > t.out[n - 1]);

		// 1. 一方向にたまらない (2 つの上限を 0 にした形では 0.07-21%)。
		CHECK(std::fabs(slope) < drift_limit(f));

		if (is_firefox_like(f)) {
			// 2. ずれは残るが 1 秒未満 (測った値は 0.41-0.73 秒。2 つの上限を 0 にした形では
			//    12-29 秒までたまった)。
			CHECK(worst < 1000000);
			// 3. 補正は上限に張り付いていない (測った値は m1 で 25-26%、m2 で 9%。2 つの上限を 0 に
			//    した形では 99%)。m1 では、W を超えるずれ (p99 で 0.4-0.5 秒) を戻す間に上限に届く
			//    フレームが 23-27% あるが、ずれはたまっていない (条件 1)。ほぼ全部のフレームで
			//    頭打ちになる状態と区別するしきい値として、半分にする。
			CHECK(capped < 0.5);
		}

		if (f.regular_again_at > 0) {
			// 4. 等間隔に戻ったら、60 秒以内に 10 ms 未満に戻る (測った値は 9 秒で入り、以後は最大
			//    2.4 ms。2 つの上限を 0 にした形では 310 秒かかり、60 秒後にもまだ 3.7 秒ずれていた)。
			const size_t at = f.regular_again_at;
			REQUIRE(at < f.ts_us.size());
			int64_t after = 0;
			for (size_t n = at; n < f.ts_us.size(); n++)
				if (f.ts_us[n] - f.ts_us[at] >= 60000000)
					after = std::max(after, std::llabs(t.out[n] - f.ts_us[n]));
			CAPTURE(after);
			CHECK(after < 10000);
		}
	}

	// 5. 2 つの上限を 0 にした形では、同じ列で 1 が失敗する (2 つの上限が動作していることを確認する)。
	for (const Fixture &f : cordmux::tests::vfr_fixtures()) {
		CAPTURE(f.name);
		const Trace t = run(f, before_plan_c());
		const double slope = drift_slope(f, t);
		CAPTURE(slope);
		CHECK(slope > drift_limit(f)); // 出力が先へ出る向きにたまる
		for (size_t n = 1; n < t.out.size(); n++)
			REQUIRE(t.out[n] > t.out[n - 1]);
	}

	// 2 つの上限は両方が必要である。
	//   - 補正の上限の割合だけ (jump_excuse_max_us = 0): 静止と動きが交互の列 (m3) で 1 が失敗する (15%)。
	//   - 差し引く量の上限だけ (max_slew_ratio = 0): Firefox を模した列 (m1) で 3 が失敗する
	//     (85% のフレームで補正が頭打ちになる)。
	{
		AlignerConfig only_slew;
		only_slew.jump_excuse_max_us = 0.0;
		const Fixture f = cordmux::tests::fixture_m3();
		const Trace t = run(f, only_slew);
		const double slope = drift_slope(f, t);
		CAPTURE(slope);
		CHECK(slope > 0.01);
	}
	for (uint64_t seed : cordmux::tests::vfr_seeds()) {
		AlignerConfig only_excuse;
		only_excuse.max_slew_ratio = 0.0;
		const Fixture f = cordmux::tests::fixture_m1(seed);
		CAPTURE(f.name);
		const Trace t = run(f, only_excuse);
		const double capped = slew_capped_ratio(t);
		CAPTURE(capped);
		CHECK(capped > 0.5);
	}
}

// ドロップの多い等間隔の列では、jump_excuse_max_us と max_slew_ratio を使っても出力のずれが変わらない
// ことを確認する。出力が W より先に出ている間は送信側の長い間隔をドロップと読まないので、実際のドロップを
// 見逃して推定に入れるおそれがある (AlignerConfig::jump_excuse_max_us のコメント)。30 fps で 5%、
// 60 fps で 10% がドロップし、ジッターが +-5 ms の列 (各 30 分) で、|ずれ| の分位点と 60 Hz の描画の結果が、
// 2 つの上限を 0 にした形と変わらないことを見る。
// 60 fps で 10% がドロップする列では、推定周期が偏って再ロックが 300 回前後起きる。ここでは、2 つの上限が
// それを悪くしていないことだけを見る。
TEST_CASE("ドロップの多い等間隔の列では、jump_excuse_max_us と max_slew_ratio を使っても出力のずれが変わらない")
{
	const Fixture fs[2] = {cordmux::tests::fixture_grid_drop(cordmux::tests::period_30(), 0.95, 5000.0, 7),
	                       cordmux::tests::fixture_grid_drop(cordmux::tests::period_60000(), 0.90, 5000.0, 8)};
	for (const Fixture &f : fs) {
		CAPTURE(f.name);
		CAPTURE(f.dropped_frames);
		REQUIRE(f.dropped_frames > 1000);
		const Trace now = run(f);
		const Trace before = run(f, before_plan_c());
		for (size_t n = 1; n < now.out.size(); n++)
			REQUIRE(now.out[n] > now.out[n - 1]);

		const double p50_now = abs_offset_quantile(f, now, 0.50);
		const double p50_before = abs_offset_quantile(f, before, 0.50);
		const double p99_now = abs_offset_quantile(f, now, 0.99);
		const double p99_before = abs_offset_quantile(f, before, 0.99);
		CAPTURE(p50_now);
		CAPTURE(p50_before);
		CAPTURE(p99_now);
		CAPTURE(p99_before);
		// 測った差は p50 で 0-0.4 ms、p99 で 0.1-0.8 ms。
		CHECK(std::fabs(p50_now - p50_before) <= 1000.0);
		CHECK(std::fabs(p99_now - p99_before) <= 2000.0);

		// 60 Hz の描画で、捨てられるフレーム (2 フレーム入る周期) は増えない (測った値は、30 fps で
		// 0 と 0、60 fps で 106 と 118)。フレームのない周期 (重複) の差は、周期の数の 0.1% 以内
		// (測った値は 0 件と 12 件)。
		const SlotResult r_now = slot_simulate(now, 600);
		const SlotResult r_before = slot_simulate(before, 600);
		CAPTURE(r_now.empty);
		CAPTURE(r_before.empty);
		CAPTURE(r_now.doubled);
		CAPTURE(r_before.doubled);
		CHECK(r_now.doubled <= r_before.doubled);
		CHECK(std::llabs(r_now.empty - r_before.empty) <= r_now.slots / 1000);
	}
}

TEST_CASE("推定周期の誤差は 600 フレーム後に 0.5% 以内、3000 フレーム後に 0.05% 以内になる")
{
	for (const Fixture &f : cordmux::tests::fixed_period_fixtures()) {
		CAPTURE(f.name);
		const Trace t = run(f);
		REQUIRE(t.period.size() > 3000);
		const double e600 = std::fabs(t.period[599] - f.true_period_us) / f.true_period_us;
		const double e3000 = std::fabs(t.period[3000] - f.true_period_us) / f.true_period_us;
		CAPTURE(t.period[599]);
		CAPTURE(e600);
		CAPTURE(t.period[3000]);
		CAPTURE(e3000);
		CHECK(e600 <= 0.005);
		CHECK(e3000 <= 0.0005);
	}
}

TEST_CASE("出力を OBS の 60 Hz の描画に割り当てると、重複と欠落が 0.15% 以下になる")
{
	// 60.000 fps ちょうどの列 (fixture a) では 0 件。
	{
		const Fixture fa = cordmux::tests::fixture_a();
		const Trace ta = run(fa);
		const SlotResult ra = slot_simulate(ta, 600);
		CAPTURE(fa.name);
		CAPTURE(ra.slots);
		CAPTURE(ra.empty);
		CAPTURE(ra.doubled);
		CHECK(ra.empty == 0);
		CHECK(ra.doubled == 0);
	}
	// 59.94 fps の列 (fixture b と c) では、59.94 fps と 60 Hz の差のために 0.1% は避けられない。
	for (const Fixture &f : {cordmux::tests::fixture_b(), cordmux::tests::fixture_c()}) {
		CAPTURE(f.name);
		const Trace t = run(f);
		const SlotResult r = slot_simulate(t, 600);
		CAPTURE(r.slots);
		CAPTURE(r.empty);
		CAPTURE(r.doubled);
		CAPTURE(r.rate);
		CHECK(r.rate <= 0.0015);
	}
}

TEST_CASE("不連続の前後でもグリッドの間隔が保たれる")
{
	for (const Fixture &f : {cordmux::tests::fixture_g(), cordmux::tests::fixture_f()}) {
		CAPTURE(f.name);
		const Trace t = run(f);
		int64_t jumps = 0;
		double worst = 0.0;
		size_t worst_at = 0;
		for (size_t n = 1; n < t.out.size(); n++) {
			if (t.step_frames[n] <= 1)
				continue;
			jumps++;
			const double expect = t.period[n] * double(t.step_frames[n]);
			const double d = std::fabs(double(t.out[n] - t.out[n - 1]) - expect);
			if (d > worst) {
				worst = d;
				worst_at = n;
			}
		}
		CAPTURE(jumps);
		CAPTURE(worst_at);
		CAPTURE(worst);
		CHECK(jumps > 0);
		CHECK(worst <= 50.0);
	}
	// fixture f の 500 ms の休止は、1 回の不連続として 30 フレームぶん進む。
	const Fixture ff = cordmux::tests::fixture_f();
	const Trace tf = run(ff);
	CHECK(tf.step_frames[1800] >= 30);
}

TEST_CASE("送信側の時計が戻っても、再整列は 1 回で済み、出力は逆行しない")
{
	// 出力が送信側より先にある状態での再整列。出力を送信側のタイムスタンプに合わせると出力が逆行し、
	// 以後は毎フレーム再整列し続ける。fixture ではこの分岐を通らないので、別に確認する。
	Fixture f = cordmux::tests::fixture_b(2000);
	for (size_t i = 1000; i < f.ts_us.size(); i++)
		f.ts_us[i] -= 300000; // 300 ms ぶん時計が戻る
	const Trace t = run(f);
	for (size_t n = 1; n < t.out.size(); n++)
		REQUIRE(t.out[n] > t.out[n - 1]);
	CAPTURE(t.final_stats.realign_count);
	CHECK(t.final_stats.realign_count == 1);
	// 補正で戻しきって、最後は定常に戻っている。
	CHECK(t.carry.back() == 0);
}

TEST_CASE("送信側の時計が 300 ms 進むと、1 回の不連続としてグリッドを進める")
{
	Fixture f = cordmux::tests::fixture_b(2000);
	for (size_t i = 1000; i < f.ts_us.size(); i++)
		f.ts_us[i] += 300000; // 300 ms ぶん時計が進む (不連続の倍率で拾える大きさ)
	const Trace t = run(f);
	// 差分 316 ms は周期の 1.5 倍を超えるので、不連続として k フレームぶん進む。
	CHECK(t.final_stats.jump_count == 1);
	for (size_t n = 1; n < t.out.size(); n++)
		REQUIRE(t.out[n] > t.out[n - 1]);
}

TEST_CASE("初期の周期のヒントを与えると立ち上がりの誤差が小さくなる")
{
	const Fixture f = cordmux::tests::fixture_b();
	AlignerConfig hinted;
	hinted.initial_period_hint_us = cordmux::tests::period_5994();
	const Trace with = run(f, hinted);
	const Trace without = run(f);
	CAPTURE(with.final_stats.err_abs_max_us);
	CAPTURE(without.final_stats.err_abs_max_us);
	CHECK(with.final_stats.err_abs_max_us <= without.final_stats.err_abs_max_us);
	CHECK(with.final_stats.realign_count == 0);
}

TEST_CASE("reset で、TimestampAligner を作った直後の状態に戻る")
{
	const Fixture f = cordmux::tests::fixture_b(1200);
	TimestampAligner a;
	for (int64_t ts : f.ts_us)
		a.align(ts);
	REQUIRE(a.stats().frames == int64_t(f.ts_us.size()));
	a.reset();
	CHECK(a.stats().frames == 0);
	CHECK(a.stats().period_us == 0.0);
	// 戻した後も、同じ列で同じ結果になる。
	std::vector<int64_t> again;
	for (int64_t ts : f.ts_us)
		again.push_back(a.align(ts));
	const Trace t = run(f);
	CHECK(again == t.out);
}

TEST_CASE("2^40 を超えるタイムスタンプでも周期と誤差が壊れない")
{
	const Fixture f = cordmux::tests::fixture_h_large();
	const Trace t = run(f);
	CHECK(t.final_stats.realign_count == 0);
	CHECK(std::fabs(t.period.back() - f.true_period_us) < f.true_period_us * 0.0005);
	// 出力が送信側から 1 周期以上離れていない。
	CHECK(std::llabs(t.out.back() - f.ts_us.back()) < int64_t(f.true_period_us));
}

TEST_CASE("補正量の累計と誤差の最大が統計に入り、補正は上限を超えない")
{
	const Fixture f = cordmux::tests::fixture_b();
	const Trace t = run(f);
	CHECK(t.final_stats.frames == int64_t(f.ts_us.size()));
	CHECK(t.final_stats.err_abs_max_us > 0.0);
	CHECK(t.final_stats.slew_abs_sum_us >= 0.0);
	// 1 フレームあたりの補正は、上限 max(500 us, 推定周期の 3%) を超えない (59.94 fps では 500.5 us)。
	const AlignerConfig cfg;
	for (size_t n = 0; n < t.slew.size(); n++) {
		const double cap = std::max(cfg.max_slew_us_per_frame, cfg.max_slew_ratio * t.period[n]);
		REQUIRE(std::fabs(t.slew[n]) <= cap + 1e-9);
	}
	// 14 fps 前後の列では、上限が推定周期の 3% まで上がり、500 us を超えて補正できる。
	// 2 つの上限を 0 にした形では、500 us で頭打ちになる。
	{
		const Fixture fm = cordmux::tests::fixture_m2();
		const Trace now = run(fm);
		const Trace before = run(fm, before_plan_c());
		double now_max = 0.0;
		double before_max = 0.0;
		for (size_t n = 0; n < now.slew.size(); n++) {
			now_max = std::max(now_max, std::fabs(now.slew[n]));
			REQUIRE(std::fabs(now.slew[n]) <= std::max(500.0, 0.03 * now.period[n]) + 1e-9);
		}
		for (double s : before.slew)
			before_max = std::max(before_max, std::fabs(s));
		CAPTURE(now_max);
		CAPTURE(before_max);
		CHECK(now_max > 1000.0);
		CHECK(before_max <= 500.0 + 1e-9);
		CHECK(now.final_stats.slew_capped_frames > 0);
	}
}

// fixture ごとの統計を表の形で標準出力に書く。値を見るためのもので、何も確認しない。
TEST_CASE("fixture ごとの整列の結果を表の形で出力する")
{
	std::printf("\n| fixture | P^ (us) | err600(%%) | err3000(%%) | realign | relock | jump | "
	            "pend | wobble | empty | doubled | dup(%%) | |err|max(ms) |\n");
	// fixture l は all_fixtures() に入っていない (推定周期の揺らぎのぶん補正が常に動作するので、出力の差分が
	// 推定周期の +-50 us に収まらない。fixtures.h のコメント)。表には含める。
	std::vector<Fixture> table = cordmux::tests::all_fixtures();
	table.push_back(cordmux::tests::fixture_l());
	for (const Fixture &f : table) {
		const Trace t = run(f);
		const double e600 = f.true_period_us > 0.0
		                            ? std::fabs(t.period[599] - f.true_period_us) /
		                                      f.true_period_us * 100.0
		                            : 0.0;
		const double e3000 = f.true_period_us > 0.0 && t.period.size() > 3000
		                             ? std::fabs(t.period[3000] - f.true_period_us) /
		                                       f.true_period_us * 100.0
		                             : 0.0;
		const SlotResult r = slot_simulate(t, 600);
		std::printf("| %s | %.3f | %.4f | %.4f | %lld | %lld | %lld | %lld | %lld | %lld | %lld | "
		            "%.4f | %.1f |\n",
		            f.name.c_str(), t.period.back(), e600, e3000,
		            (long long)t.final_stats.realign_count,
		            (long long)t.final_stats.relock_count, (long long)t.final_stats.jump_count,
		            (long long)t.final_stats.jump_pending_count,
		            (long long)t.final_stats.jump_wobble_count, (long long)r.empty,
		            (long long)r.doubled, r.rate * 100.0,
		            t.final_stats.err_abs_max_us / 1000.0);
	}
	std::printf("\n| dev | 収束フレーム数 | 最終の誤差(%%) | realign | relock | jump | 最後の 2000 フレームの余分な格子 |\n");
	for (double dev : cordmux::tests::sustained_devs()) {
		const Fixture f = cordmux::tests::fixture_i(dev, 3000, 12000);
		const Trace t = run(f);
		int conv = -1;
		for (size_t n = 3000; n < t.period.size(); n++) {
			const bool ok = std::fabs(t.period[n] - f.true_period_us) <= f.true_period_us * 0.005;
			if (!ok)
				conv = -1;
			else if (conv < 0)
				conv = int(n) - 3000;
		}
		int64_t extra = 0;
		for (size_t n = t.out.size() - 2000; n < t.out.size(); n++)
			if (t.step_frames[n] != 1)
				extra++;
		std::printf("| %+.0f%% | %d | %.4f | %lld | %lld | %lld | %lld |\n", dev * 100.0, conv,
		            std::fabs(t.final_stats.period_us - f.true_period_us) / f.true_period_us * 100.0,
		            (long long)t.final_stats.realign_count, (long long)t.final_stats.relock_count,
		            (long long)t.final_stats.jump_count, (long long)extra);
	}
	// 不連続を確認する手順 (jump_confirm_frames) の有無で、同じ列の結果がどう変わるか。
	std::printf("\n| fixture j | jump | pend | wobble | 寄せの累計(ms) | empty | doubled | dup(%%) |\n");
	for (int confirm : {0, 1}) {
		AlignerConfig cfg;
		cfg.jump_confirm_frames = confirm;
		const Fixture f = cordmux::tests::fixture_j(108000, cordmux::tests::window_seeds()[0]);
		const Trace t = run(f, cfg);
		const SlotResult r = slot_simulate(t, 600);
		std::printf("| jump_confirm_frames = %d | %lld | %lld | %lld | %.0f | %lld | %lld | %.4f |\n",
		            confirm, (long long)t.final_stats.jump_count,
		            (long long)t.final_stats.jump_pending_count,
		            (long long)t.final_stats.jump_wobble_count,
		            t.final_stats.slew_abs_sum_us / 1000.0, (long long)r.empty,
		            (long long)r.doubled, r.rate * 100.0);
	}
	// 外れ値を除く手順 (relock_trim) の有無で、fixture l の結果がどう変わるか。
	std::printf("\n| fixture l | relock | jump | 飛びで進めたフレーム数 / 抜けの数 | empty | doubled |\n");
	for (int trim : {0, 1}) {
		AlignerConfig cfg;
		cfg.relock_trim = trim;
		const Fixture f = cordmux::tests::fixture_l();
		const Trace t = run(f, cfg);
		const SlotResult r = slot_simulate(t, 600);
		std::printf("| relock_trim = %d | %lld | %lld | %lld / %lld | %lld | %lld |\n", trim,
		            (long long)t.final_stats.relock_count, (long long)t.final_stats.jump_count,
		            (long long)t.final_stats.jump_frames, (long long)f.dropped_frames,
		            (long long)r.empty, (long long)r.doubled);
	}
	// 変動フレームレートの列で、2 つの上限を 0 にした形、片方だけの形、既定の形を比べる。
	std::printf("\n| fixture | 形 | 傾き(%%) | 最後のずれ(ms) | |ずれ| p99(ms) | |ずれ| 最大(ms) | "
	            "寄せの張り付き(%%) | 見逃しの頭打ち | realign | relock | jump |\n");
	{
		AlignerConfig only_slew;
		only_slew.jump_excuse_max_us = 0.0;
		AlignerConfig only_excuse;
		only_excuse.max_slew_ratio = 0.0;
		const AlignerConfig shapes[4] = {before_plan_c(), only_slew, only_excuse, AlignerConfig{}};
		const char *names[4] = {"案 C の前", "寄せの割合だけ", "見逃しの上限だけ", "案 C (既定)"};
		for (const Fixture &f : cordmux::tests::vfr_fixtures()) {
			for (int s = 0; s < 4; s++) {
				const Trace t = run(f, shapes[s]);
				std::printf("| %s | %s | %.5f | %.1f | %.1f | %.1f | %.1f | %lld | %lld | %lld | %lld |\n",
				            f.name.c_str(), names[s], drift_slope(f, t) * 100.0,
				            double(t.out.back() - f.ts_us.back()) / 1000.0,
				            abs_offset_quantile(f, t, 0.99) / 1000.0,
				            double(max_abs_offset(f, t)) / 1000.0, slew_capped_ratio(t) * 100.0,
				            (long long)t.final_stats.jump_excuse_capped_frames,
				            (long long)t.final_stats.realign_count,
				            (long long)t.final_stats.relock_count, (long long)t.final_stats.jump_count);
			}
		}
	}
	// m5 で等間隔に戻った後の、ずれの戻り方。
	std::printf("\n| m5 | 形 | 戻った時のずれ(ms) | 10 ms 未満に入って以後出ないまで(秒) | 60 秒より後の |ずれ| 最大(ms) |\n");
	{
		const Fixture f = cordmux::tests::fixture_m5();
		const size_t at = f.regular_again_at;
		for (int s = 0; s < 2; s++) {
			const Trace t = run(f, s == 0 ? before_plan_c() : AlignerConfig{});
			double settle_s = -1.0;
			int64_t after = 0;
			for (size_t n = at; n < f.ts_us.size(); n++) {
				const int64_t off = std::llabs(t.out[n] - f.ts_us[n]);
				if (off >= 10000)
					settle_s = -1.0;
				else if (settle_s < 0.0)
					settle_s = double(f.ts_us[n] - f.ts_us[at]) / 1e6;
				if (f.ts_us[n] - f.ts_us[at] >= 60000000)
					after = std::max(after, off);
			}
			std::printf("| %s | %s | %.1f | %.1f | %.1f |\n", f.name.c_str(),
			            s == 0 ? "案 C の前" : "案 C (既定)", double(t.out[at] - f.ts_us[at]) / 1000.0,
			            settle_s, double(after) / 1000.0);
		}
	}
	// ドロップの多い等間隔の列で、2 つの上限を 0 にした形と既定の形を比べる。
	std::printf("\n| 列 | 形 | |ずれ| p50(ms) | |ずれ| p99(ms) | 抜け | 飛びで進めたフレーム数 | relock | "
	            "realign | empty | doubled |\n");
	{
		const Fixture fs[2] = {
		        cordmux::tests::fixture_grid_drop(cordmux::tests::period_30(), 0.95, 5000.0, 7),
		        cordmux::tests::fixture_grid_drop(cordmux::tests::period_60000(), 0.90, 5000.0, 8)};
		for (const Fixture &f : fs) {
			for (int s = 0; s < 2; s++) {
				const Trace t = run(f, s == 0 ? before_plan_c() : AlignerConfig{});
				const SlotResult r = slot_simulate(t, 600);
				std::printf("| %s | %s | %.1f | %.1f | %lld | %lld | %lld | %lld | %lld | %lld |\n",
				            f.name.c_str(), s == 0 ? "案 C の前" : "案 C (既定)",
				            abs_offset_quantile(f, t, 0.50) / 1000.0,
				            abs_offset_quantile(f, t, 0.99) / 1000.0, (long long)f.dropped_frames,
				            (long long)t.final_stats.jump_frames, (long long)t.final_stats.relock_count,
				            (long long)t.final_stats.realign_count, (long long)r.empty,
				            (long long)r.doubled);
			}
		}
	}
	std::printf("\n");
	CHECK(true);
}
