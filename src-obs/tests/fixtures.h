// SPDX-License-Identifier: Apache-2.0

// TimestampAligner (align/timestamp_aligner.h) の単体テストに使う、タイムスタンプの列 (fixture) を作る。乱数は決まった
// 種から作るので、どの環境でも同じ列になる。標準ライブラリの分布 (uniform_real_distribution など) は実装によって値が
// 変わるので、乱数も分布も自分で持つ。ジッターの幅 +-5 ms は、送信側のタイムスタンプで測った誤差 (平均 2 ms、最大
// 5 から 12 ms) に合わせてある。

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace cordmux::tests {

// xorshift64* (同じ種からは同じ列を返す)。
class Rng {
public:
	explicit Rng(uint64_t seed) : s_(seed ? seed : 1u) {}

	uint64_t next()
	{
		s_ ^= s_ >> 12;
		s_ ^= s_ << 25;
		s_ ^= s_ >> 27;
		return s_ * 2685821657736338717ull;
	}

	// [0, 1)
	double uniform01() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }

	// [-amp, +amp]
	double uniform_sym(double amp) { return (uniform01() * 2.0 - 1.0) * amp; }

	// 平均 0、標準偏差 sigma (Box-Muller)。
	double normal(double sigma)
	{
		double u1 = uniform01();
		if (u1 < 1e-12)
			u1 = 1e-12;
		const double u2 = uniform01();
		return sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
	}

private:
	uint64_t s_;
};

struct Fixture {
	std::string name;
	std::vector<int64_t> ts_us;   // 送信側のタイムスタンプ
	double true_period_us = 0.0;  // 実際の周期 (フレームレートが変わる列では 0)
	std::string note;
	// 送信側でドロップしたフレーム (実際のドロップ) の数と、ドロップの直後のフレームの番号 (この列の中での番号)。
	// ドロップを入れた fixture だけが値を入れる。
	int64_t dropped_frames = 0;
	std::vector<size_t> after_drop{};
	// 等間隔でない区間が終わり、等間隔に戻った最初のフレームの番号 (fixture_m5 だけが値を入れる)。
	size_t regular_again_at = 0;
};

// 実際の周期 (us)。59.94 は 60000/1001 fps のことである。
inline double period_60000() { return 1000000.0 / 60.0; }          // 16666.6667
inline double period_5994() { return 1001000000.0 / 60000.0; }     // 16683.3167
inline double period_30() { return 1000000.0 / 30.0; }             // 33333.3333

enum class Jitter { None, Uniform, Normal };

// 周期が一定の列。origin は最初のタイムスタンプ。
inline std::vector<int64_t> make_cfr(double period_us, int frames, Jitter kind, double amp,
                                     uint64_t seed, int64_t origin)
{
	Rng rng(seed);
	std::vector<int64_t> ts;
	ts.reserve(size_t(frames));
	for (int n = 0; n < frames; n++) {
		double j = 0.0;
		if (kind == Jitter::Uniform)
			j = rng.uniform_sym(amp);
		else if (kind == Jitter::Normal)
			j = rng.normal(amp);
		ts.push_back(origin + int64_t(std::llround(double(n) * period_us + j)));
	}
	return ts;
}

// (a) 60.000 fps ちょうどで、ジッターがない。
inline Fixture fixture_a(int frames = 3600)
{
	Fixture f;
	f.name = "a: 60.000 fps ちょうど (揺れ 0)";
	f.true_period_us = period_60000();
	f.ts_us = make_cfr(f.true_period_us, frames, Jitter::None, 0.0, 1001, 0);
	return f;
}

// (b) 59.94 fps と、一様分布のジッター +-5 ms。
inline Fixture fixture_b(int frames = 3600)
{
	Fixture f;
	f.name = "b: 59.94 fps + 一様な揺れ +-5 ms";
	f.true_period_us = period_5994();
	f.ts_us = make_cfr(f.true_period_us, frames, Jitter::Uniform, 5000.0, 20260912, 0);
	return f;
}

// (c) 59.94 fps と、正規分布のジッター (標準偏差 2 ms)。
inline Fixture fixture_c(int frames = 3600)
{
	Fixture f;
	f.name = "c: 59.94 fps + 正規の揺れ sigma 2 ms";
	f.true_period_us = period_5994();
	f.ts_us = make_cfr(f.true_period_us, frames, Jitter::Normal, 2000.0, 777001, 0);
	return f;
}

// (d) 30 fps と、一様分布のジッター +-5 ms。
inline Fixture fixture_d(int frames = 3600)
{
	Fixture f;
	f.name = "d: 30 fps + 一様な揺れ +-5 ms";
	f.true_period_us = period_30();
	f.ts_us = make_cfr(f.true_period_us, frames, Jitter::Uniform, 5000.0, 30303, 0);
	return f;
}

// (e) フレームレートが 30、60、30 fps と変わる (各 600 フレーム)。
// 切り替えそのものを確認する fixture なので、ジッターは +-1 ms にしている。+-5 ms のジッターのまま周期が変わる場合は、
// fixture_i が確認する。
inline Fixture fixture_e(int seg = 600)
{
	Fixture f;
	f.name = "e: 可変 30 -> 60 -> 30 (各 600 フレーム)";
	f.true_period_us = 0.0;
	f.note = "relock 2 回";
	Rng rng(555444);
	int64_t t = 0;
	const double segs[3] = {period_30(), period_60000(), period_30()};
	for (int s = 0; s < 3; s++) {
		for (int n = 0; n < seg; n++) {
			const double j = rng.uniform_sym(1000.0);
			f.ts_us.push_back(int64_t(std::llround(double(t) + j)));
			t += int64_t(std::llround(segs[s]));
		}
	}
	return f;
}

// (f) 97 フレームに 1 フレームがドロップし、途中に 500 ms の停止がある。ドロップの判定そのものを確認する fixture なので、
// ジッターは入れない。
inline Fixture fixture_f(int frames = 3600)
{
	Fixture f;
	f.name = "f: 抜け (97 フレームに 1 フレーム) + 500 ms の休止";
	f.true_period_us = period_5994();
	f.note = "休止は 1800 フレーム目";
	int64_t extra = 0;
	int emitted = 0;
	for (int n = 0; emitted < frames; n++) {
		if (n > 0 && n % 97 == 0)
			continue; // このフレームはドロップする
		if (emitted == 1800)
			extra += 500000; // 500 ms の停止
		f.ts_us.push_back(int64_t(std::llround(double(n) * f.true_period_us)) + extra);
		emitted++;
	}
	return f;
}

// (g) 3000 フレーム目で、タイムスタンプが 2 秒進む不連続 (再接続を模したもの)。
inline Fixture fixture_g(int frames = 3600)
{
	Fixture f;
	f.name = "g: 3000 フレーム目に 2 秒の跳び (繋ぎ直しの模擬)";
	f.true_period_us = period_5994();
	f.ts_us = make_cfr(f.true_period_us, frames, Jitter::None, 0.0, 909, 0);
	for (size_t i = 3000; i < f.ts_us.size(); i++)
		f.ts_us[i] += 2000000;
	return f;
}

// (h-1) 最初のタイムスタンプが負の値。
inline Fixture fixture_h_negative(int frames = 3600)
{
	Fixture f;
	f.name = "h1: 開始が負の値 (-2e9 us)";
	f.true_period_us = period_5994();
	f.ts_us = make_cfr(f.true_period_us, frames, Jitter::Uniform, 5000.0, 424242, -2000000000ll);
	return f;
}

// (h-2) 最初のタイムスタンプが 2^40 を超える値。
inline Fixture fixture_h_large(int frames = 3600)
{
	Fixture f;
	f.name = "h2: 開始が 2^40 + 1 us";
	f.true_period_us = period_5994();
	f.ts_us = make_cfr(f.true_period_us, frames, Jitter::Uniform, 5000.0, 424243,
	                   (1ll << 40) + 1ll);
	return f;
}

// (i) 60 fps から、持続的に別のフレームレートに変わる (ジッターは、ほかの fixture と同じ +-5 ms)。
// dev = (変化後の周期 - 60 fps の周期) / 60 fps の周期。
//
// fixture_e はジッターを +-1 ms にしているので、+-5 ms のジッターのまま周期が変わる場合は、この fixture で確認する。
// dev が relock_deviation (20%) より小さいときは再ロックが起きず、回帰だけで推定周期が新しい周期に近づく。推定周期が
// 変化前の値の近くで止まらないことを確認するために、dev を何通りも取る (sustained_devs)。
inline Fixture fixture_i(double dev, int phase1 = 3000, int phase2 = 12000, uint64_t seed = 601500)
{
	Fixture f;
	const double p1 = period_60000();
	const double p2 = p1 * (1.0 + dev);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "i: 60 fps から持続的に dev %+.0f%% (揺れ +-5 ms)", dev * 100.0);
	f.name = buf;
	f.true_period_us = p2; // 変化後の周期 (収束先)
	f.note = "ジッター +-5 ms のまま周期が変わる列";
	Rng rng(seed);
	int64_t t = 0;
	const double segs[2] = {p1, p2};
	const int lens[2] = {phase1, phase2};
	for (int s = 0; s < 2; s++) {
		for (int n = 0; n < lens[s]; n++) {
			const double j = rng.uniform_sym(5000.0);
			f.ts_us.push_back(int64_t(std::llround(double(t) + j)));
			t += int64_t(std::llround(segs[s]));
		}
	}
	return f;
}

// (j) 60 fps と一様分布のジッター +-5 ms の列に、15 秒ごとに、遅れて届いたフレームの次が早く届くジッターを入れる。
// 30 分 (108000 フレーム) で、送信側では 1 フレームもドロップしていない。
//
// テスト用の送信側 (Electron、60 fps、システム時刻のタイムスタンプ) の列には、差分が 25 ms の次に 8 ms になる単発の
// ジッターがあった (2 フレームの合計は 2 周期のまま)。1 フレームのタイムスタンプだけが遅れ、次のフレームは元の位置に
// 戻る (次のフレームのタイムスタンプは動かさない)。この形を、不連続 (実際のドロップ) と誤判定しないことを確認する。
//
// 測った遅延は 8.3 ms (0.5 周期) だったが、ここでは 10 から 25 ms の遅延を入れ、1 周期に近く、実際の 1 フレームの
// ドロップと見分けにくい側を確認する。ただし送信側のタイムスタンプは必ず増え続けるので、次のフレームとの間隔が 1 ms
// 残るように遅延を抑える (ジッターによっては 5.7 ms まで下がる)。15 秒ごと (900 フレームごと) で、30 分に 119 回入る。
inline Fixture fixture_j(int frames = 108000, uint64_t seed = 20260913)
{
	Fixture f;
	f.name = "j: 60 fps 揺れ +-5 ms + 15 秒ごとの遅れて来て次が早い";
	f.true_period_us = period_60000();
	f.note = "送信側でドロップしていない列";
	Rng rng(seed);
	std::vector<double> jit(size_t(frames), 0.0);
	for (int n = 0; n < frames; n++)
		jit[size_t(n)] = rng.uniform_sym(5000.0);
	const int every = 900; // 15 秒 (60 fps)
	for (int n = every; n + 1 < frames; n += every) {
		double late = 10000.0 + rng.uniform01() * 15000.0; // 10 から 25 ms
		const double room = f.true_period_us + jit[size_t(n) + 1] - jit[size_t(n)] - 1000.0;
		if (late > room)
			late = room;
		if (late > 0.0)
			jit[size_t(n)] += late;
	}
	f.ts_us.reserve(size_t(frames));
	for (int n = 0; n < frames; n++)
		f.ts_us.push_back(int64_t(std::llround(double(n) * f.true_period_us + jit[size_t(n)])));
	return f;
}

// (k) (j) に、実際のドロップ (97 フレームに 1 フレーム) を加えたもの。ジッターを不連続と誤判定しないまま、実際の
// ドロップをすべて不連続として数えることを確認する。
inline Fixture fixture_k(int frames = 108000, uint64_t seed = 20260913)
{
	Fixture src = fixture_j(frames, seed);
	Fixture f;
	f.name = "k: (j) + 実際の抜け (97 フレームに 1 フレーム)";
	f.true_period_us = src.true_period_us;
	f.note = "ドロップをすべて不連続として数える列";
	f.ts_us.reserve(src.ts_us.size());
	bool gap = false;
	for (size_t n = 0; n < src.ts_us.size(); n++) {
		if (n > 0 && n % 97 == 0) {
			gap = true; // このフレームはドロップする
			continue;
		}
		if (gap) {
			f.after_drop.push_back(f.ts_us.size());
			f.dropped_frames++;
			gap = false;
		}
		f.ts_us.push_back(src.ts_us[n]);
	}
	return f;
}

// (l) (j) に、2 フレーム連続の実際のドロップを加えたもの。fixture_k はドロップを 1 フレームずつ入れる (97 フレームに
// 1 フレーム)。ここでは同じ 97 フレームごとの位置で、2 フレーム (n と n+1) を続けてドロップする。
//
// 2 フレーム連続のドロップの差分は 3 周期なので、直近 8 フレームの差分に 1 つ入るだけで平均が 0.25 周期上がり、20% の
// しきい値を超える。残りの差分がジッターで偶然同じ向きにずれると、再ロックの条件がそろう。そのため再ロックの判定では、
// 平均を取る前に外れ値を 1 つ除く (AlignerConfig::relock_trim)。これを確認するのはテスト「ジッターと 2 フレーム連続の
// ドロップが重なっても、再ロックしない」で、比べるために relock_trim を 0 にしても実行する。
inline Fixture fixture_l(int frames = 108000, uint64_t seed = 20260913)
{
	Fixture src = fixture_j(frames, seed);
	Fixture f;
	f.name = "l: (j) + 実際の抜けが 2 フレーム連続 (97 フレーム周期)";
	f.true_period_us = src.true_period_us;
	f.note = "ドロップが続く列";
	f.ts_us.reserve(src.ts_us.size());
	int64_t gap = 0; // 直前に続けてドロップしたフレームの数
	for (size_t n = 0; n < src.ts_us.size(); n++) {
		const size_t m = n % 97;
		if (n > 0 && (m == 0 || m == 1)) {
			gap++; // このフレームはドロップする (2 フレーム連続のドロップ)
			continue;
		}
		if (gap > 0) {
			f.after_drop.push_back(f.ts_us.size());
			f.dropped_frames += gap;
			gap = 0;
		}
		f.ts_us.push_back(src.ts_us[n]);
	}
	return f;
}

// ---------------------------------------------------------------------------
// (m1 から m5) 変動フレームレートの列。出力が送信側のタイムスタンプより先に進むずれが、一方向にたまらないことを
// 確認する。
//
// Firefox から画面共有した映像では、推定周期が 50 から 148 ms の間で変わる (平均 14 fps 前後、間隔は 17 から 188 ms で
// 不規則)。フレームごとのタイムスタンプは記録していないので、下の列はどれも合成した列 (実際のデータではない) で、測った
// 統計に近づけて作った (推定周期 47 から 125 ms、再ロック 0.12 回/秒は実際とほぼ同じで、不連続とジッターは実際より多い)。
//
// 最初のタイムスタンプ (us)。値に意味はないが、システム時刻と同じ桁 (2^50 を超える) にして、大きな値でも計算が壊れない
// ことも確認する。
inline int64_t vfr_origin_us() { return 1789375929837000ll; }

// 対数正規分布の間隔 (平均 mean_us、変動係数 cv)。下限は 1 ms (送信側のタイムスタンプは必ず増えるため)。
inline double lognormal_interval(Rng &rng, double mean_us, double cv)
{
	const double s2 = std::log(1.0 + cv * cv);
	const double mu = std::log(mean_us) - s2 / 2.0;
	double iv = std::exp(mu + rng.normal(std::sqrt(s2)));
	if (iv < 1000.0)
		iv = 1000.0;
	return iv;
}

// 区間ごとにフレームレートを [8, 20] fps から一様に選び直し (区間は 3 から 20 秒)、区間の中の間隔を対数正規分布
// (変動係数 cv) にした列を ts に追加する。t_us は列の先頭からの経過時間 (us) で、追加し終えた位置を返す。
inline double append_firefox_like(std::vector<int64_t> &ts, double t_us, double minutes, double cv,
                                  Rng &rng)
{
	const double end = t_us + minutes * 60e6;
	while (t_us < end) {
		const double fps = 8.0 + rng.uniform01() * 12.0;
		const double seg_end = t_us + (3.0 + rng.uniform01() * 17.0) * 1e6;
		while (t_us < seg_end && t_us < end) {
			ts.push_back(vfr_origin_us() + int64_t(std::llround(t_us)));
			t_us += lognormal_interval(rng, 1e6 / fps, cv);
		}
	}
	return t_us;
}

// (m1) Firefox から画面共有した映像を模した列 (60 分)。区間ごとに 8 から 20 fps で、区間は 3 から 20 秒、区間の中の
// 間隔は対数正規分布 (変動係数 0.25)。
inline Fixture fixture_m1(uint64_t seed = 51, double cv = 0.25, double minutes = 60.0)
{
	Fixture f;
	char buf[128];
	std::snprintf(buf, sizeof(buf), "m1: 区間ごとに 8-20 fps + 対数正規 CV %.2f (%.0f 分、種 %llu)", cv,
	              minutes, (unsigned long long)seed);
	f.name = buf;
	f.note = "Firefox から画面共有した映像を模した合成の列";
	Rng rng(seed);
	append_firefox_like(f.ts_us, 0.0, minutes, cv, rng);
	return f;
}

// (m2) 区間を持たない対数正規分布の間隔 (平均 72 ms で 14 fps、変動係数 0.2、60 分)。
inline Fixture fixture_m2(uint64_t seed = 32, double minutes = 60.0)
{
	Fixture f;
	char buf[128];
	std::snprintf(buf, sizeof(buf), "m2: 対数正規の間隔 平均 72 ms CV 0.2 (%.0f 分、種 %llu)", minutes,
	              (unsigned long long)seed);
	f.name = buf;
	f.note = "格子に乗らない間隔だけの列";
	Rng rng(seed);
	double t = 0.0;
	const double end = minutes * 60e6;
	while (t < end) {
		f.ts_us.push_back(vfr_origin_us() + int64_t(std::llround(t)));
		t += lognormal_interval(rng, 72000.0, 0.2);
	}
	return f;
}

// (m3) 30 fps の動きの 1 から 3 秒と、1 fps の静止の 5 から 20 秒が交互に来る列 (ジッター +-2 ms、既定は 120 分)。
// 実際には観測していない。静止した画面にときどき動きが入る内容を配信すると仮定した列である。
// 120 分にするのは、この列では出力と送信側のずれが jump_excuse_max_us を超えて 4 秒前後残り (TimestampAligner の
// コメント)、それが 10 分ごとの平均から求める回帰の傾きに入るためである。30 分では、乱数の種によって傾きが +-0.02% まで
// 変わり、0.01% の判定が種で決まってしまう (8 通りの種で測った値。120 分では最大 0.0011%)。
inline Fixture fixture_m3(uint64_t seed = 78, double minutes = 120.0)
{
	Fixture f;
	char buf[128];
	std::snprintf(buf, sizeof(buf), "m3: 30 fps の動き 1-3 秒 + 1 fps の静止 5-20 秒 (%.0f 分、種 %llu)",
	              minutes, (unsigned long long)seed);
	f.name = buf;
	f.note = "仮定の列 (実際には観測していない)";
	Rng rng(seed);
	double t = 0.0;
	const double end = minutes * 60e6;
	while (t < end) {
		const double burst_end = t + (1.0 + rng.uniform01() * 2.0) * 1e6;
		for (; t < burst_end; t += 1e6 / 30.0)
			f.ts_us.push_back(vfr_origin_us() + int64_t(std::llround(t + rng.uniform_sym(2000.0))));
		const double still_end = t + (5.0 + rng.uniform01() * 15.0) * 1e6;
		for (; t < still_end; t += 1e6)
			f.ts_us.push_back(vfr_origin_us() + int64_t(std::llround(t + rng.uniform_sym(2000.0))));
	}
	return f;
}

// (m4) まばらな列。間隔が 0.1 から 2 秒の一様分布 (変化したときだけフレームが来る画面、既定は 120 分)。m3 と同じく
// 仮定の列である。120 分にする理由も m3 と同じ (この列のずれは最大 9.6 秒あり、30 分では、傾きが種によって +-0.11% まで
// 変わる。120 分では最大 0.0125%)。
inline Fixture fixture_m4(uint64_t seed = 77, double minutes = 120.0)
{
	Fixture f;
	char buf[128];
	std::snprintf(buf, sizeof(buf), "m4: まばら 0.1-2 秒の一様な間隔 (%.0f 分、種 %llu)", minutes,
	              (unsigned long long)seed);
	f.name = buf;
	f.note = "仮定の列 (実際には観測していない)";
	Rng rng(seed);
	double t = 0.0;
	const double end = minutes * 60e6;
	while (t < end) {
		f.ts_us.push_back(vfr_origin_us() + int64_t(std::llround(t)));
		t += 100000.0 + rng.uniform01() * 1900000.0;
	}
	return f;
}

// (m5) 30 fps (ジッター +-2 ms) の 10 分、m1 と同じ形の等間隔でない列の 10 分、30 fps の 10 分の順に並べた列。等間隔に
// 戻った後に、たまったずれが補正で戻ることを確認する。デスクトップ版の Discord から画面共有した映像でも、間隔が乱れる
// 区間で同じ向きのずれが生じる (推定周期 55 ms のときに 1.9 秒のずれを測った)。
inline Fixture fixture_m5(uint64_t seed = 41)
{
	Fixture f;
	char buf[128];
	std::snprintf(buf, sizeof(buf), "m5: 30 fps 10 分 -> 等間隔でない 10 分 -> 30 fps 10 分 (種 %llu)",
	              (unsigned long long)seed);
	f.name = buf;
	f.note = "等間隔へ戻った後の戻り方を見る列";
	Rng rng(seed);
	const double p30 = 1e6 / 30.0;
	double t = 0.0;
	for (; t < 600e6; t += p30)
		f.ts_us.push_back(vfr_origin_us() + int64_t(std::llround(t + rng.uniform_sym(2000.0))));
	t = append_firefox_like(f.ts_us, t, 10.0, 0.25, rng);
	f.regular_again_at = f.ts_us.size();
	for (; t < 1800e6; t += p30)
		f.ts_us.push_back(vfr_origin_us() + int64_t(std::llround(t + rng.uniform_sym(2000.0))));
	return f;
}

// m1 を作るときの乱数の種 (3 通り)。
inline std::vector<uint64_t> vfr_seeds()
{
	return {51ull, 52ull, 53ull};
}

// 変動フレームレートの列 (テスト「変動フレームレートの列で、出力のずれが一方向にたまらない」が使う)。all_fixtures() には
// 入れない。等間隔のグリッドを前提にしたテスト「定常では出力の差分が推定周期の +-50 us に収まる」は、等間隔でない列では
// 意味を持たないためである。
inline std::vector<Fixture> vfr_fixtures()
{
	std::vector<Fixture> v;
	for (uint64_t seed : vfr_seeds())
		v.push_back(fixture_m1(seed));
	v.push_back(fixture_m2());
	v.push_back(fixture_m3());
	v.push_back(fixture_m4());
	v.push_back(fixture_m5());
	return v;
}

// ドロップの多い等間隔の列。周期 period_us のグリッドの位置ごとに確率 keep でフレームを残し、一様分布のジッター
// +-jit_us を加える。jump_excuse_max_us と max_slew_ratio を使っても結果が変わらないことを確認する (テスト
// 「ドロップの多い等間隔の列では、jump_excuse_max_us と max_slew_ratio を使っても出力のずれが変わらない」)。
inline Fixture fixture_grid_drop(double period_us, double keep, double jit_us, uint64_t seed,
                                 double minutes = 30.0)
{
	Fixture f;
	char buf[128];
	std::snprintf(buf, sizeof(buf), "%.0f fps の升目から %.0f%% 抜ける + 揺れ +-%.0f ms (%.0f 分)",
	              1e6 / period_us, (1.0 - keep) * 100.0, jit_us / 1000.0, minutes);
	f.name = buf;
	f.true_period_us = period_us;
	Rng rng(seed);
	const double end = minutes * 60e6;
	for (int64_t n = 0;; n++) {
		const double t = double(n) * period_us;
		if (t > end)
			break;
		if (n != 0 && rng.uniform01() >= keep) {
			f.dropped_frames++;
			continue;
		}
		f.ts_us.push_back(vfr_origin_us() + int64_t(std::llround(t + rng.uniform_sym(jit_us))));
	}
	return f;
}

// fixture_i に渡す dev の値。再ロックが起きない側 (dev < 0.20) と、起きる側の両方を確認する。
inline std::vector<double> sustained_devs()
{
	return {0.02, 0.05, 0.10, 0.15, 0.20, 0.25, -0.20};
}

// ジッターだけの fixture (再整列も再ロックも 0 回でなければならないもの)。
inline std::vector<Fixture> jitter_only_fixtures()
{
	return {fixture_a(), fixture_b(), fixture_c(), fixture_d(), fixture_h_negative(),
	        fixture_h_large()};
}

// 実際の周期が決まっている fixture (周期の推定の精度を確認するもの)。
inline std::vector<Fixture> fixed_period_fixtures()
{
	return {fixture_a(), fixture_b(), fixture_c(), fixture_d(),
	        fixture_f(), fixture_g(), fixture_h_negative(), fixture_h_large()};
}

// すべての fixture。
// j と k は 30 分 (108000 フレーム) あるので、3 通りの種で実行するテスト (「遅れて届いたフレームの次が早く届く
// ジッターを、不連続と誤判定しない」と「実際の 1 フレームのドロップは全部不連続として数え、余分な欠落や重複を
// 作らない」) とは別に、ここでは既定の種の 1 つだけを入れる (単調であること、定常の差分、表の出力のため)。
//
// l (2 フレーム連続のドロップ) はここに入れない。ドロップとジッターが重なる列では、推定周期が実際の値の周りで +-0.13%
// 変動し、その誤差を戻す補正が定常でも常に働くので、テスト「定常では出力の差分が推定周期の +-50 us に収まる」の範囲に
// 収まらない (測ると 64 us)。l を確認するのはテスト「ジッターと 2 フレーム連続のドロップが重なっても、再ロックしない」
// で、テスト「fixture ごとの整列の結果を表の形で出力する」は l の行も出力する。
inline std::vector<Fixture> all_fixtures()
{
	return {fixture_a(), fixture_b(), fixture_c(),          fixture_d(),
	        fixture_e(), fixture_f(), fixture_g(),          fixture_h_negative(),
	        fixture_h_large(), fixture_j(), fixture_k()};
}

// fixture_j、fixture_k、fixture_l を 3 通りに作るときの乱数の種。
inline std::vector<uint64_t> window_seeds()
{
	return {20260913ull, 31337ull, 987654ull};
}

} // namespace cordmux::tests
