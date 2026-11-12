// SPDX-License-Identifier: Apache-2.0

// 音のチャンクを、チャンクごとの目標の位置 (呼び出し元の時刻から決めたサンプルの位置) に追従させて書き出す
// (AudioDriftCorrector)。音を取り出す時計と出力の時計の速さの差を、ごくわずかなサンプルレートの変換で連続的に、
// 両方の向きで吸収する。AudioMixer (配信) と録画が同じものを使う。決め方は docs/architecture.md の
// 「音声の時計の差の吸収」にある。
// libobs にも FFmpeg にも依存しない。C++17 の標準ライブラリと、x64 では SSE2 の組み込み関数 (emmintrin.h) だけを
// 使う。スレッドセーフではない。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define CORDMUX_AUDIO_DRIFT_SSE2 1
#endif

namespace cordmux::align {

// 追従のふるまいを決める値。既定の値の理由は項目ごとに書く。make_audio_drift_config が設定の値から作る。
struct AudioDriftConfig {
	// サンプルレート (Hz)。目標の位置と出力の位置は、このレートのサンプルの番号で数える。
	int sample_rate = 48000;

	// 置き直しのしきい値 (us)。目標と、続けて書いたときの位置の差がこれを超えたら、本当の不連続 (共有の始め直し、
	// 受信の途切れ、時刻の飛び) とみなして目標の位置へ置き直す。超えない差は速さの補正でゆっくり戻す。
	// 既定の 45 ms は、音と映像のずれに人が気付き始める音の先行の量 (ITU-R BT.1359-1 の +45 ms) である。これより
	// 大きい差を何秒もかけて戻すと、その間ずれが見えるので置き直す。小さい差 (上流の数 ms から 20 ms の段、
	// 音の装置の周期でまとめて届くチャンクの時刻の揺れ) は置き直さない。置き直すと音が途切れるか重なるためである。
	// 時刻の揺れの大きさは音の装置と PC の負荷で変わるので、設定 (audio_resync_ms) で変えられる。
	std::int64_t resync_us = 45000;

	// 速さの補正の上限 (ppm)。補正の量は音の高さの変化として聞こえる。1000 ppm は 1.7 セントで、音楽でも聞き分けにくい
	// 量にした。時計どうしの速さの差 (水晶の精度で数十 ppm) はこの内に収まり、残りで段や揺れを戻す。上限を上げると
	// 段を早く戻せるが、高さの変化が聞こえやすくなる。この得失は利用者が選ぶので、設定 (audio_drift_max_ppm) で変えられる。
	double max_ppm = 1000.0;

	// 追従の時定数 (us)。目標との差を速さの補正に変える比例と積分の強さを決める (比例 2 / 時定数、積分 1 / 時定数の 2 乗。
	// 減衰が臨界になる組)。長いほどチャンクごとの時刻の揺れを強くならすが、段を戻すのが遅くなる。8 秒にすると、
	// 2 ms の揺れが補正の速さに残す揺れは数十 ppm で、上限 (1000 ppm) より十分に小さい。揺れの大きさは環境で
	// 変わるので、設定 (audio_drift_time_constant_ms) で変えられる。差をならす指数移動平均の時定数は、その 1/8 にする
	// (追従の時定数より十分に短くないと、ならしの遅れで追従が振動する)。
	std::int64_t time_constant_us = 8000000;
};

// 設定の値 (ms と ppm) から AudioDriftConfig を作る。配信 (pipeline/) と録画 (record/) が同じ値を同じ形で使うために、
// ここ 1 か所で作る。0 以下の値は、それぞれ 1 として扱う。
inline AudioDriftConfig make_audio_drift_config(int resync_ms, int max_ppm, int time_constant_ms)
{
	AudioDriftConfig config;
	config.resync_us = std::int64_t(std::max(resync_ms, 1)) * 1000;
	config.max_ppm = double(std::max(max_ppm, 1));
	config.time_constant_us = std::int64_t(std::max(time_constant_ms, 1)) * 1000;
	return config;
}

// push の 1 回分の結果。
struct AudioDriftStep {
	// 目標の位置へ置き直したか (最初のチャンク、reset の後、しきい値を超えた差、書く位置が遅すぎたとき)。
	bool resynced = false;
	// この push で書き出す最初のサンプルの位置。置き直さなかったときは、前の push で書き出した最後のサンプルの次である。
	std::int64_t start_pos = 0;
	// この push で書き出すサンプルの数。変換の計算に、入力の位置の後ろの kAudioDriftHalfTaps サンプルが要るので、
	// 書き出しは入力より少し遅れる (書き出しきれなかった分は次の push で書き出す)。
	std::size_t frames = 0;
	// 置き直さなかったときの、目標の位置 - 続けて書いたときの位置 (us)。正は、目標が後ろにあることを示す。
	double error_us = 0.0;
};

// 統計。呼び出し元がログの統計の行に書く。
struct AudioDriftStats {
	std::uint64_t resyncs = 0;     // 置き直した回数 (最初のチャンクと reset の後の置き直しは数えない)
	double drift_ppm = 0.0;        // 推定した、時計どうしの速さの差 (積分の値)
	double rate_ppm = 0.0;         // 直近の push で使った速さの補正
	double max_abs_error_us = 0.0; // 置き直さなかった push の |error_us| の最大
};

// 変換の計算に使う、入力の位置の前後のサンプルの数 (片側)。窓をかけた sinc を前後 16 サンプルずつ (32 タップ)
// 使う。32 タップは、48 kHz で 20 kHz までの振幅をそろえ、位置の端数による音の変化を聞こえない大きさにするための
// 長さである。Kaiser の窓の遷移の幅の見積もりでは、32 タップは 20 kHz 付近から、16 タップは 17 kHz 付近から上の振幅が
// 下がる。
inline constexpr int kAudioDriftHalfTaps = 16;

// 目標の位置への追従を決める部分 (サンプルの値は扱わない)。AudioDriftCorrector が使う。位置だけを計算するので、
// 長い時間の追従を、サンプルを作らずに単体テストで確かめられる。
//
// 位置の決め方。入力のサンプルの番号を x (置き直してから数える。小数も取る)、出力のサンプルの位置を p とし、出力の
// j 番目 (置き直してから数える) は p = 置き直した位置 + j、x = x0 + (j の前までに使った入力の進み) にある。1 つの push の
// 間の入力の進みは 1 / 比 で一定である。続けて書いたときに次の入力のサンプル (x = 受け取った入力の数) が来る位置と、
// その push の目標の位置の差を、比例と積分で比に戻す (時計どうしの速さの一定の差は、積分が持つので位置の差が 0 に戻る)。
class AudioDriftTracker {
public:
	AudioDriftTracker() = default;
	explicit AudioDriftTracker(const AudioDriftConfig &config) : config_(config) {}

	// 入力のチャンク 1 つ (frames サンプル) を受け取り、書き出す位置と数を決める。
	//   target_pos  このチャンクの最初のサンプルを置きたい位置 (出力のサンプルの番号。小数も取る)
	//   min_pos     書き出してよい最も前の位置。続けて書くと、この位置より前から書き出すことになり、目標が続けて
	//               書く位置より後ろにあるときは、置き直す (配信で、出し終えた位置より前に書くと音が捨てられるため)。
	//               値がなければ確かめない
	AudioDriftStep push(double target_pos, std::size_t frames, std::optional<std::int64_t> min_pos = std::nullopt)
	{
		AudioDriftStep step;
		const double rate = double(config_.sample_rate > 0 ? config_.sample_rate : 1);
		bool resync = !started_ || force_resync_;
		double error = 0.0;
		if (!resync) {
			const double next = next_input_pos();
			error = target_pos - next;
			const double resync_samples = double(config_.resync_us) * rate / 1e6;
			if (std::fabs(error) > resync_samples) {
				resync = true;
			} else if (min_pos && base_ + emitted_ < *min_pos && target_pos > next) {
				resync = true;
			}
		}

		if (resync) {
			if (started_ && !force_resync_)
				++stats_.resyncs;
			started_ = true;
			force_resync_ = false;
			// 置き直すと差は 0 から数え直すので、比は推定した時計どうしの速さの差だけにする。
			ratio_ = 1.0 + drift_;
			// 出力の位置は整数なので、置き直した位置は目標の四捨五入にし、端数は入力の位置の初めの値に移す
			// (出力の 0 番目は、入力の -0.5 から 0.5 の位置にある。0 より前の入力は無音として扱う)。
			base_ = std::llround(target_pos);
			emitted_ = 0;
			received_ = 0;
			const double x0 = (double(base_) - target_pos) / ratio_;
			read_int_ = std::int64_t(std::floor(x0));
			read_frac_ = x0 - double(read_int_);
			smoothed_s_ = 0.0;
			step.resynced = true;
		} else {
			update_ratio(error / rate, double(frames) / rate);
			step.error_us = error * 1e6 / rate;
			stats_.max_abs_error_us = std::max(stats_.max_abs_error_us, std::fabs(step.error_us));
		}

		received_ += std::int64_t(frames);
		step.start_pos = base_ + emitted_;
		const double inc = 1.0 / ratio_;
		first_int_ = read_int_;
		first_frac_ = read_frac_;
		last_inc_ = inc;
		const std::int64_t count = ready_count(inc);
		const double end = read_frac_ + double(count) * inc;
		const double carry = std::floor(end);
		read_int_ += std::int64_t(carry);
		read_frac_ = end - carry;
		emitted_ += count;
		step.frames = std::size_t(count);
		stats_.rate_ppm = (ratio_ - 1.0) * 1e6;
		stats_.drift_ppm = drift_ * 1e6;
		return step;
	}

	// 次の push を必ず目標の位置へ置き直す。推定した速さの差は残す (時計どうしの差は、音が途切れても変わらない)。
	void reset() { force_resync_ = true; }

	// 1 つでもチャンクを受け取ったか。
	bool started() const { return started_; }

	// 次の push に target_pos を渡したら、しきい値を超えた差で置き直すか (状態は変えない)。最初のチャンクと reset の
	// 後は真。録画が、書いたサンプルより前に置き直すことになるかを、push の前に確かめるのに使う。
	bool would_resync(double target_pos) const
	{
		if (!started_ || force_resync_)
			return true;
		const double rate = double(config_.sample_rate > 0 ? config_.sample_rate : 1);
		return std::fabs(target_pos - next_input_pos()) > double(config_.resync_us) * rate / 1e6;
	}

	// 置き直さずに続けたときに、次の入力のサンプルが来る位置。まだ何も受け取っていなければ 0。
	double next_input_pos() const
	{
		const double pending = double(received_ - read_int_) - read_frac_;
		return double(base_ + emitted_) + pending * ratio_;
	}

	// 直近の push で書き出したサンプルの、入力の位置 (最初の 1 つの整数部と端数) と 1 つごとの進み。j 番目の位置は
	// 整数部 + (端数 + j x 進み) で、AudioDriftCorrector がサンプルの値を計算するのに使う (足し続けずに掛け算で
	// 求めるので、ここで数えた位置と 1 ビットも違わない)。
	std::int64_t first_input_int() const { return first_int_; }
	// 次に書き出すサンプルの入力の位置の整数部。
	std::int64_t next_read_int() const { return read_int_; }
	double first_input_frac() const { return first_frac_; }
	double input_step() const { return last_inc_; }

	const AudioDriftStats &stats() const { return stats_; }
	const AudioDriftConfig &config() const { return config_; }

private:
	// 差 (秒) とチャンクの長さ (秒) から比を決める。
	void update_ratio(double error_s, double dt_s)
	{
		const double tau = double(std::max<std::int64_t>(config_.time_constant_us, 1)) / 1e6;
		const double smooth_tau = tau / 8.0;
		smoothed_s_ += (error_s - smoothed_s_) * (dt_s / (smooth_tau + dt_s));
		// 半サンプル以内の差では比を動かさない。目標はマイクロ秒に丸めた時刻から作るので、その丸め (0.024 サンプル)
		// とならした後に残る揺れで比を動かすと、時刻がそろった入力でもサンプルの値を変換し続けることになる。
		const double deadband_s = 0.5 / double(config_.sample_rate > 0 ? config_.sample_rate : 1);
		double e = 0.0;
		if (smoothed_s_ > deadband_s)
			e = smoothed_s_ - deadband_s;
		else if (smoothed_s_ < -deadband_s)
			e = smoothed_s_ + deadband_s;
		const double max = config_.max_ppm * 1e-6;
		const double kp = 2.0 / tau;
		const double ki = 1.0 / (tau * tau);
		const double proposed = drift_ + kp * e;
		// 補正が上限に張り付き、差が同じ向きに押している間は積分しない (積分が上限の外まで積もると、差が戻った
		// 後に行き過ぎる)。
		const bool saturated = (proposed >= max && e > 0.0) || (proposed <= -max && e < 0.0);
		if (!saturated)
			drift_ = std::clamp(drift_ + ki * e * dt_s, -max, max);
		ratio_ = 1.0 + std::clamp(drift_ + kp * e, -max, max);
	}

	// 受け取った入力で計算できる出力の数。j 番目の出力 (入力の位置 read_int_ + read_frac_ + j x inc) の計算には、
	// その位置の後ろ kAudioDriftHalfTaps サンプルまでが要る。比が 1 で端数が 0 の間は入力のサンプルそのものなので、
	// 後ろのサンプルを待たない。
	std::int64_t ready_count(double inc) const
	{
		const bool exact = read_frac_ == 0.0 && inc == 1.0;
		const std::int64_t limit = received_ - read_int_ - (exact ? 0 : kAudioDriftHalfTaps);
		// j 番目が計算できるのは、read_frac_ + j x inc < limit のとき (limit は整数なので、整数部で比べるのと同じ)。
		const auto ok = [&](std::int64_t j) { return read_frac_ + double(j) * inc < double(limit); };
		std::int64_t j = std::int64_t(std::floor((double(limit) - read_frac_) / inc)) + 1;
		if (j < 0)
			j = 0;
		while (j > 0 && !ok(j - 1))
			--j;
		while (ok(j))
			++j;
		return j;
	}

	AudioDriftConfig config_{};
	AudioDriftStats stats_{};
	bool started_ = false;
	bool force_resync_ = false;
	std::int64_t base_ = 0;      // 置き直した位置 (出力の 0 番目の位置)
	std::int64_t emitted_ = 0;   // 置き直してから書き出した数
	std::int64_t received_ = 0;  // 置き直してから受け取った入力の数
	// 次に書き出すサンプルの入力の位置。10 時間で 17 億サンプルになるので、double 1 つで足し続けると丸めの誤差が
	// 積もる。整数部と端数に分けて持つ。
	std::int64_t read_int_ = 0;
	double read_frac_ = 0.0;
	std::int64_t first_int_ = 0;
	double first_frac_ = 0.0;
	double last_inc_ = 1.0;
	double ratio_ = 1.0;         // 出力のサンプル数 / 入力のサンプル数
	double drift_ = 0.0;         // 積分 (速さの差の推定)
	double smoothed_s_ = 0.0;    // ならした差 (秒)
};

namespace detail {

// 窓をかけた sinc の係数の表。位相 (入力の位置の端数) を kAudioDriftPhases に分け、隣の位相の間は直線で補う。
// 窓は Kaiser (beta 7) で、位相ごとに係数の和を 1 にそろえる (端数によって音量が変わらないため)。
inline constexpr int kAudioDriftPhases = 256;
inline constexpr int kAudioDriftTaps = kAudioDriftHalfTaps * 2;
// 表の 1 行の長さ。位相 p の係数 (kAudioDriftTaps 個) と、位相 p + 1 の係数との差 (kAudioDriftTaps 個) を並べる。
// 差を前もって作っておくと、サンプルごとの直線の補間は掛け算と足し算 1 回ずつになる。1 つのサンプルに要る係数が
// 続いた 256 バイトに収まるので、キャッシュの行も 4 つで済む。
inline constexpr int kAudioDriftRow = kAudioDriftTaps * 2;

inline double bessel_i0(double x)
{
	double sum = 1.0;
	double term = 1.0;
	for (int k = 1; k < 40; ++k) {
		const double h = x / (2.0 * double(k));
		term *= h * h;
		sum += term;
		if (term < sum * 1e-17)
			break;
	}
	return sum;
}

// 係数の表。位相 p の行は p x kAudioDriftRow 番目から。行の先頭を 64 バイト (キャッシュの行) にそろえる。SSE2 の
// 読み込みが 16 バイトにそろうと、読み込みを掛け算と足し算の命令の中に入れられるので、命令が減る。
struct AudioDriftKernelTable {
	alignas(64) float v[std::size_t(kAudioDriftPhases) * kAudioDriftRow];
};

inline const float *audio_drift_kernel()
{
	static const std::unique_ptr<AudioDriftKernelTable> table = [] {
		constexpr double kPi = 3.14159265358979323846;
		constexpr double kBeta = 7.0;
		std::vector<float> coef(std::size_t(kAudioDriftPhases + 1) * kAudioDriftTaps);
		const double i0_beta = bessel_i0(kBeta);
		for (int p = 0; p <= kAudioDriftPhases; ++p) {
			const double frac = double(p) / double(kAudioDriftPhases);
			double row[kAudioDriftTaps];
			double sum = 0.0;
			for (int k = 0; k < kAudioDriftTaps; ++k) {
				// k 番目のタップは入力の (整数部 - 15 + k) 番目のサンプルで、出力の位置からの距離は
				// frac + 15 - k。
				const double t_dist = frac + double(kAudioDriftHalfTaps - 1 - k);
				const double sinc = t_dist == 0.0 ? 1.0 : std::sin(kPi * t_dist) / (kPi * t_dist);
				const double r = t_dist / double(kAudioDriftHalfTaps);
				const double w =
					std::fabs(r) >= 1.0 ? 0.0 : bessel_i0(kBeta * std::sqrt(1.0 - r * r)) / i0_beta;
				row[k] = sinc * w;
				sum += row[k];
			}
			for (int k = 0; k < kAudioDriftTaps; ++k)
				coef[std::size_t(p) * kAudioDriftTaps + std::size_t(k)] = float(row[k] / sum);
		}
		auto t = std::make_unique<AudioDriftKernelTable>();
		for (int p = 0; p < kAudioDriftPhases; ++p) {
			const float *row0 = &coef[std::size_t(p) * kAudioDriftTaps];
			const float *row1 = row0 + kAudioDriftTaps;
			float *out = &t->v[std::size_t(p) * kAudioDriftRow];
			for (int k = 0; k < kAudioDriftTaps; ++k) {
				out[k] = row0[k];
				out[kAudioDriftTaps + k] = row1[k] - row0[k];
			}
		}
		return t;
	}();
	return table->v;
}

// 1 つの出力 (j 番目) の位置。first は 32 タップの最初の入力の番号 (base + 整数部)、phase は位相の行 (-1 は入力の
// サンプルそのもの)、frac は位相の間の端数。std::floor と同じ値の整数部を、整数への変換 (0 の向きへの切り捨て) と、
// 負の数だけ 1 引くことで求める (x64 の SSE2 には端数を切り捨てる命令がなく、std::floor は関数の呼び出しになるため)。
inline void audio_drift_position(double f0, double inc, std::int64_t base, std::size_t j, std::int64_t *first,
				 int *phase, float *frac)
{
	const double pos = f0 + double(j) * inc;
	std::int64_t whole = std::int64_t(pos);
	if (double(whole) > pos)
		--whole;
	const double f = pos - double(whole);
	*first = base + whole;
	if (f == 0.0 && inc == 1.0) {
		*phase = -1;
		*frac = 0.0f;
		return;
	}
	const double p = f * double(kAudioDriftPhases);
	int pi = int(p);
	if (pi >= kAudioDriftPhases)
		pi = kAudioDriftPhases - 1;
	*phase = pi;
	*frac = float(p - double(pi));
}

// 変換の計算は 2 つの実装を持つ。x64 では SSE2 の組み込み関数を使う AudioDriftSse2Ops、それ以外では組み込み関数を
// 使わない AudioDriftPortableOps を使う (AudioDriftDefaultOps)。どちらも同じ関数を持つ。
//   coef       位相の行と端数から、32 個の係数を作る
//   dot        係数と入力の 32 サンプルの内積
//   stereo     2 チャンネルの 1 つの出力のサンプルを、インターリーブの 2 つのサンプルとして書く
//   positions  j0 番目から n 個の出力の位置 (audio_drift_position と同じ値)。配列は n を偶数に切り上げた長さを持つ
// 32 タップの和は、タップを 4 つずつの 8 つの組 (0 から 7) に分け、組 g と組 g + 4 の積の和を 4 つ (g = 0 から 3) 作り、
// (0 + 2) + (1 + 3) と足す。その 4 つの値 (組の中の位置ごと) も (0 + 2) + (1 + 3) と足す。1 つの和に順に足すと、
// 足し算が前の結果を待つ鎖 (32 段) になり、そこで時間が決まるので、鎖を短くする。2 つの実装はこの順で足すので、
// MSVC の x64 では同じ値になる。テスト (test_align_audio_drift_ops.cpp) は、2 つの実装の位置が 1 ビットも違わないことと、
// 値の差が丸めの誤差の見積もりの上限以内であることを確かめる。
struct AudioDriftPortableOps {
	struct Coef {
		float v[kAudioDriftTaps];
	};

	static void coef(const float *row, float pf, Coef *c)
	{
		for (int k = 0; k < kAudioDriftTaps; ++k)
			c->v[k] = row[k] + row[kAudioDriftTaps + k] * pf;
	}

	static float dot(const Coef &c, const float *src)
	{
		float s[4];
		for (int m = 0; m < 4; ++m) {
			float a[4];
			for (int g = 0; g < 4; ++g)
				a[g] = c.v[4 * g + m] * src[4 * g + m] + c.v[4 * g + 16 + m] * src[4 * g + 16 + m];
			s[m] = (a[0] + a[2]) + (a[1] + a[3]);
		}
		return (s[0] + s[2]) + (s[1] + s[3]);
	}

	static void stereo(const float *row, float pf, const float *left, const float *right, float *dst)
	{
		Coef c;
		coef(row, pf, &c);
		dst[0] = dot(c, left);
		dst[1] = dot(c, right);
	}

	static void positions(double f0, double inc, std::int64_t base, std::size_t j0, std::size_t n,
			      std::int64_t *first, int *phase, float *frac)
	{
		for (std::size_t b = 0; b < n; ++b)
			audio_drift_position(f0, inc, base, j0 + b, &first[b], &phase[b], &frac[b]);
	}
};

#if defined(CORDMUX_AUDIO_DRIFT_SSE2)

struct AudioDriftSse2Ops {
	// 係数 (kAudioDriftTaps 個) を 4 つずつ持つ。
	struct Coef {
		__m128 v[kAudioDriftTaps / 4];
	};

	// 係数の組 g (row は表の行で、16 バイトにそろっている。f は位相の間の端数を 4 つ並べたもの)。
	static __m128 coef4(const float *row, __m128 f, int g)
	{
		return _mm_add_ps(_mm_load_ps(row + 4 * g), _mm_mul_ps(_mm_load_ps(row + kAudioDriftTaps + 4 * g), f));
	}

	static void coef(const float *row, float pf, Coef *c)
	{
		const __m128 f = _mm_set1_ps(pf);
		for (int g = 0; g < kAudioDriftTaps / 4; ++g)
			c->v[g] = coef4(row, f, g);
	}

	// 組の中の位置ごとの 4 つの和。
	static __m128 dot4(const Coef &c, const float *src)
	{
		__m128 a[4];
		for (int g = 0; g < 4; ++g)
			a[g] = _mm_add_ps(_mm_mul_ps(c.v[g], _mm_loadu_ps(src + 4 * g)),
					  _mm_mul_ps(c.v[g + 4], _mm_loadu_ps(src + 4 * g + 16)));
		return _mm_add_ps(_mm_add_ps(a[0], a[2]), _mm_add_ps(a[1], a[3]));
	}

	static float dot(const Coef &c, const float *src)
	{
		const __m128 s = dot4(c, src);
		const __m128 t = _mm_add_ps(s, _mm_movehl_ps(s, s));
		return _mm_cvtss_f32(_mm_add_ss(t, _mm_shuffle_ps(t, t, 1)));
	}

	// 係数は作ったその場で両方のチャンネルに使う (係数を一度メモリーに置くより、レジスターだけで済む)。ループで
	// 書くと MSVC が展開せず、係数を配列の添字で読むことになるので、8 つの組を並べて書く。
	static void stereo(const float *row, float pf, const float *left, const float *right, float *dst)
	{
		const __m128 f = _mm_set1_ps(pf);
		__m128 c = coef4(row, f, 0);
		__m128 l0 = _mm_mul_ps(c, _mm_loadu_ps(left));
		__m128 r0 = _mm_mul_ps(c, _mm_loadu_ps(right));
		c = coef4(row, f, 1);
		__m128 l1 = _mm_mul_ps(c, _mm_loadu_ps(left + 4));
		__m128 r1 = _mm_mul_ps(c, _mm_loadu_ps(right + 4));
		c = coef4(row, f, 2);
		__m128 l2 = _mm_mul_ps(c, _mm_loadu_ps(left + 8));
		__m128 r2 = _mm_mul_ps(c, _mm_loadu_ps(right + 8));
		c = coef4(row, f, 3);
		__m128 l3 = _mm_mul_ps(c, _mm_loadu_ps(left + 12));
		__m128 r3 = _mm_mul_ps(c, _mm_loadu_ps(right + 12));
		c = coef4(row, f, 4);
		l0 = _mm_add_ps(l0, _mm_mul_ps(c, _mm_loadu_ps(left + 16)));
		r0 = _mm_add_ps(r0, _mm_mul_ps(c, _mm_loadu_ps(right + 16)));
		c = coef4(row, f, 5);
		l1 = _mm_add_ps(l1, _mm_mul_ps(c, _mm_loadu_ps(left + 20)));
		r1 = _mm_add_ps(r1, _mm_mul_ps(c, _mm_loadu_ps(right + 20)));
		c = coef4(row, f, 6);
		l2 = _mm_add_ps(l2, _mm_mul_ps(c, _mm_loadu_ps(left + 24)));
		r2 = _mm_add_ps(r2, _mm_mul_ps(c, _mm_loadu_ps(right + 24)));
		c = coef4(row, f, 7);
		l3 = _mm_add_ps(l3, _mm_mul_ps(c, _mm_loadu_ps(left + 28)));
		r3 = _mm_add_ps(r3, _mm_mul_ps(c, _mm_loadu_ps(right + 28)));
		const __m128 l = _mm_add_ps(_mm_add_ps(l0, l2), _mm_add_ps(l1, l3));
		const __m128 r = _mm_add_ps(_mm_add_ps(r0, r2), _mm_add_ps(r1, r3));
		// (l0 + l2, r0 + r2, l1 + l3, r1 + r3)
		const __m128 t = _mm_add_ps(_mm_unpacklo_ps(l, r), _mm_unpackhi_ps(l, r));
		_mm_storel_pi(reinterpret_cast<__m64 *>(dst), _mm_add_ps(t, _mm_movehl_ps(t, t)));
	}

	// 位置を 2 つずつ、倍精度の命令で求める。整数部を 32 ビットで扱うので、位置の絶対値 (1 + (j0 + n + 1) x |inc|
	// より小さい) が 2^31 に届きうるときは、1 つずつ求める (1 回のチャンクの長さとしては届かない)。
	static void positions(double f0, double inc, std::int64_t base, std::size_t j0, std::size_t n,
			      std::int64_t *first, int *phase, float *frac)
	{
		if (!(std::fabs(double(j0 + n + 1) * inc) < 1073741824.0)) {
			AudioDriftPortableOps::positions(f0, inc, base, j0, n, first, phase, frac);
			return;
		}
		const __m128d vf0 = _mm_set1_pd(f0);
		const __m128d vinc = _mm_set1_pd(inc);
		const __m128d one = _mm_set1_pd(1.0);
		const __m128d two = _mm_set1_pd(2.0);
		const __m128d phases = _mm_set1_pd(double(kAudioDriftPhases));
		const __m128i last = _mm_set1_epi32(kAudioDriftPhases - 1);
		const bool unit = inc == 1.0;
		__m128d jd = _mm_set_pd(double(j0 + 1), double(j0));
		for (std::size_t b = 0; b < n; b += 2, jd = _mm_add_pd(jd, two)) {
			const __m128d pos = _mm_add_pd(vf0, _mm_mul_pd(jd, vinc));
			// 0 の向きへ切り捨て、切り捨てた値が pos より大きい (pos が負) ときは 1 引く。
			__m128d whole = _mm_cvtepi32_pd(_mm_cvttpd_epi32(pos));
			whole = _mm_sub_pd(whole, _mm_and_pd(_mm_cmpgt_pd(whole, pos), one));
			const __m128i wi = _mm_cvttpd_epi32(whole);
			const __m128d f = _mm_sub_pd(pos, whole);
			const __m128d p = _mm_mul_pd(f, phases);
			__m128i pi = _mm_cvttpd_epi32(p);
			pi = _mm_add_epi32(pi, _mm_cmpgt_epi32(pi, last)); // kAudioDriftPhases 以上は 1 引く
			const __m128 pf = _mm_cvtpd_ps(_mm_sub_pd(p, _mm_cvtepi32_pd(pi)));
			first[b] = base + _mm_cvtsi128_si32(wi);
			first[b + 1] = base + _mm_cvtsi128_si32(_mm_srli_si128(wi, 4));
			_mm_storel_epi64(reinterpret_cast<__m128i *>(phase + b), pi);
			_mm_storel_pi(reinterpret_cast<__m64 *>(frac + b), pf);
			if (unit) {
				const int zero = _mm_movemask_pd(_mm_cmpeq_pd(f, _mm_setzero_pd()));
				for (int k = 0; k < 2; ++k) {
					if (zero & (1 << k)) {
						phase[b + std::size_t(k)] = -1;
						frac[b + std::size_t(k)] = 0.0f;
					}
				}
			}
		}
	}
};

using AudioDriftDefaultOps = AudioDriftSse2Ops;

#else

using AudioDriftDefaultOps = AudioDriftPortableOps;

#endif

} // namespace detail

// 音のチャンクを目標の位置に追従させ、変換したサンプルを書き出す。AudioDriftTracker が位置を決め、ここで窓をかけた
// sinc でサンプルの値を計算する。チャンネル数は作るときに決める (受け取るサンプルと書き出すサンプルはインターリーブ)。
// 受け取った入力はチャンネルごとに分けて持つ。1 つのチャンネルの 32 タップが続いた場所に並ぶので、4 つずつまとめて
// 掛けて足せる。
//
// Ops は変換の計算の実装 (detail の AudioDriftSse2Ops か AudioDriftPortableOps)。製品は既定の計算の
// AudioDriftCorrector を使い、テストは組み込み関数を使わない計算で作ったものと出力を比べる。マクロで切り替えないのは、
// 同じ名前のクラスの中身が翻訳単位ごとに違うと (ODR の違反)、リンクでどちらか一方だけが残り、2 つを比べたつもりで
// 同じものを比べることになるためである。
//
// FFmpeg の libswresample (swr_set_compensation) を使わないのは、次の 2 つのためである。(1) 変換の内部にためる
// サンプルの数が呼び出しごとに変わり、書き出したサンプルが入力のどの位置に当たるかを、ここで決めた位置と 1 サンプルの
// 単位で突き合わせる必要がある。(2) 配信と録画の両方の単体テストで、時刻の追従を FFmpeg なしで確かめたい。
template<class Ops> class BasicAudioDriftCorrector {
public:
	explicit BasicAudioDriftCorrector(const AudioDriftConfig &config = AudioDriftConfig{}, std::size_t channels = 2)
		: tracker_(config),
		  channels_(channels > 0 ? channels : 1),
		  history_(channels_)
	{
	}

	// チャンクを 1 つ受け取り、書き出すサンプルを out に入れる (out は上書きする)。samples は frames x チャンネル数の
	// インターリーブ。書き出す位置と数は戻り値にある (AudioDriftTracker::push)。置き直したときは、前のチャンクの
	// 書き出しきれなかった末尾 (kAudioDriftHalfTaps サンプル以下) を捨てる。
	AudioDriftStep push(double target_pos, const float *samples, std::size_t frames, std::vector<float> *out,
			    std::optional<std::int64_t> min_pos = std::nullopt)
	{
		const AudioDriftStep step = tracker_.push(target_pos, frames, min_pos);
		if (step.resynced) {
			length_ = 0;
			history_start_ = 0;
		}
		append(samples, frames);
		render(step.frames, out);
		return step;
	}

	void reset() { tracker_.reset(); }

	const AudioDriftTracker &tracker() const { return tracker_; }
	const AudioDriftStats &stats() const { return tracker_.stats(); }

private:
	// 受け取ったサンプルを、チャンネルごとの履歴の末尾に追加する。領域は足りないときだけ広げ、0 で埋め直さない。
	void append(const float *samples, std::size_t frames)
	{
		const std::size_t old = length_;
		length_ = old + frames;
		if (length_ > history_[0].size()) {
			for (std::vector<float> &h : history_)
				h.resize(std::max(length_, h.size() * 2));
		}
		if (channels_ == 2) {
			float *left = history_[0].data() + old;
			float *right = history_[1].data() + old;
			for (std::size_t f = 0; f < frames; ++f) {
				left[f] = samples[f * 2];
				right[f] = samples[f * 2 + 1];
			}
			return;
		}
		for (std::size_t c = 0; c < channels_; ++c) {
			float *dst = history_[c].data() + old;
			for (std::size_t f = 0; f < frames; ++f)
				dst[f] = samples[f * channels_ + c];
		}
	}

	// 入力の i 番目のサンプル (チャンネル c)。受け取る前と、置き直す前の番号 (負) は無音にする。
	float input_at(std::int64_t i, std::size_t c) const
	{
		if (i < history_start_)
			return 0.0f;
		const std::size_t index = std::size_t(i - history_start_);
		return index < length_ ? history_[c][index] : 0.0f;
	}

	void render(std::size_t count, std::vector<float> *out)
	{
		// 下で全部のサンプルを書くので、0 で埋めない。
		out->resize(count * channels_);
		const std::int64_t i0 = tracker_.first_input_int();
		const double f0 = tracker_.first_input_frac();
		const double inc = tracker_.input_step();
		if (f0 == 0.0 && inc == 1.0)
			copy_exact(i0, count, out->data());
		else
			convert(i0, f0, inc, count, out->data());
		discard_old();
	}

	// 比が 1 で端数がない間は、j 番目は入力の i0 + j 番目のサンプルそのもの。
	void copy_exact(std::int64_t i0, std::size_t count, float *dst) const
	{
		// メンバーをループの前で変数に移す (convert_block と同じ理由)。
		const std::size_t channels = channels_;
		const std::int64_t start = history_start_;
		const std::size_t length = length_;
		const float *left = history_[0].data();
		const float *right = channels == 2 ? history_[1].data() : nullptr;
		for (std::size_t j = 0; j < count; ++j, dst += channels) {
			const std::int64_t i = i0 + std::int64_t(j);
			if (i < start || std::size_t(i - start) >= length) {
				for (std::size_t c = 0; c < channels; ++c)
					dst[c] = 0.0f;
				continue;
			}
			const std::size_t at = std::size_t(i - start);
			if (right != nullptr) {
				dst[0] = left[at];
				dst[1] = right[at];
				continue;
			}
			for (std::size_t c = 0; c < channels; ++c)
				dst[c] = history_[c][at];
		}
	}

	// kBlock 個ずつ、先に全部の位置 (入力の整数部と位相) を求め、その後で値を計算する。1 つのサンプルの中で位置から
	// 値までを続けて計算すると、位置の変換と係数と内積が前の結果を待つ長い鎖になり、CPU が次のサンプルの計算を
	// 重ねきれない。位置だけのループと値だけのループに分けると、それぞれが重なる。
	void convert(std::int64_t i0, double f0, double inc, std::size_t count, float *dst) const
	{
		constexpr std::size_t kBlock = 64; // 偶数 (Ops::positions が 2 つずつ書くため)
		std::int64_t firsts[kBlock];       // 32 タップの最初の入力の番号
		int phases[kBlock];                // 位相の行の番号。-1 は、入力のサンプルそのもの (端数がない)
		float fracs[kBlock];               // 位相の間の端数
		const std::int64_t base = i0 - (kAudioDriftHalfTaps - 1);
		for (std::size_t j0 = 0; j0 < count; j0 += kBlock) {
			const std::size_t n = std::min(kBlock, count - j0);
			Ops::positions(f0, inc, base, j0, n, firsts, phases, fracs);
			convert_block(firsts, phases, fracs, n, dst + j0 * channels_);
		}
	}

	// 位置を求めた n 個の出力の値を計算する。
	void convert_block(const std::int64_t *firsts, const int *phases, const float *fracs, std::size_t n,
			   float *dst) const
	{
		// メンバーをループの前で変数に移す (dst への書き込みがメンバーを書き換えうるとみなされ、毎回メモリーから
		// 読まないようにするため)。
		const std::size_t channels = channels_;
		const std::int64_t start = history_start_;
		const std::size_t length = length_;
		const float *left = history_[0].data();
		const float *right = channels == 2 ? history_[1].data() : nullptr;
		const float *kernel = detail::audio_drift_kernel();
		for (std::size_t b = 0; b < n; ++b, dst += channels) {
			const std::int64_t first = firsts[b];
			if (phases[b] < 0) {
				for (std::size_t c = 0; c < channels; ++c)
					dst[c] = input_at(first + (kAudioDriftHalfTaps - 1), c);
				continue;
			}
			const float *row = kernel + std::size_t(phases[b]) * detail::kAudioDriftRow;
			const bool inside = first >= start &&
					    std::size_t(first - start) + std::size_t(detail::kAudioDriftTaps) <= length;
			if (inside && right != nullptr) {
				const std::size_t at = std::size_t(first - start);
				Ops::stereo(row, fracs[b], left + at, right + at, dst);
				continue;
			}
			typename Ops::Coef coef;
			Ops::coef(row, fracs[b], &coef);
			for (std::size_t c = 0; c < channels; ++c) {
				if (inside) {
					dst[c] = Ops::dot(coef, history_[c].data() + (first - start));
					continue;
				}
				// 置き直した直後の、受け取る前の番号を含む所。無音を詰めた 32 サンプルで同じ計算をする。
				float window[detail::kAudioDriftTaps];
				for (int k = 0; k < detail::kAudioDriftTaps; ++k)
					window[k] = input_at(first + k, c);
				dst[c] = Ops::dot(coef, window);
			}
		}
	}

	// 次の計算に要らない古いサンプルを捨てる (次に書き出す位置の前 kAudioDriftHalfTaps サンプルは残す)。
	void discard_old()
	{
		const std::int64_t keep_from = tracker_.next_read_int() - kAudioDriftHalfTaps;
		if (keep_from <= history_start_)
			return;
		const std::size_t drop = std::min(std::size_t(keep_from - history_start_), length_);
		for (std::vector<float> &h : history_)
			std::copy(h.begin() + std::ptrdiff_t(drop), h.begin() + std::ptrdiff_t(length_), h.begin());
		length_ -= drop;
		history_start_ += std::int64_t(drop);
	}

	AudioDriftTracker tracker_;
	std::size_t channels_;
	// チャンネルごとの、受け取った入力。history_start_ 番目から length_ 個。領域の長さは length_ 以上。
	std::vector<std::vector<float>> history_;
	std::size_t length_ = 0;
	std::int64_t history_start_ = 0;
};

// 配信 (AudioMixer) と録画が使う、既定の計算の AudioDriftCorrector。
using AudioDriftCorrector = BasicAudioDriftCorrector<detail::AudioDriftDefaultOps>;

} // namespace cordmux::align

#undef CORDMUX_AUDIO_DRIFT_SSE2
