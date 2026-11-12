// SPDX-License-Identifier: Apache-2.0

// 音のチャンクを、チャンクごとの目標の位置 (呼び出し元の時刻から決めたサンプルの位置) に追従させて書き出す
// (AudioDriftCorrector)。音を取り出す時計と出力の時計の速さの差を、ごくわずかなサンプルレートの変換で連続的に、
// 両方の向きで吸収する。AudioMixer (配信) と録画が同じものを使う。決め方は docs/architecture.md の
// 「音声の時計の差の吸収」にある。
// libobs にも FFmpeg にも依存しない。C++17 の標準ライブラリだけを使う。スレッドセーフではない。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

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

inline const std::vector<float> &audio_drift_kernel()
{
	static const std::vector<float> table = [] {
		constexpr double kPi = 3.14159265358979323846;
		constexpr double kBeta = 7.0;
		std::vector<float> t(std::size_t(kAudioDriftPhases + 1) * kAudioDriftTaps);
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
				t[std::size_t(p) * kAudioDriftTaps + std::size_t(k)] = float(row[k] / sum);
		}
		return t;
	}();
	return table;
}

} // namespace detail

// 音のチャンクを目標の位置に追従させ、変換したサンプルを書き出す。AudioDriftTracker が位置を決め、ここで窓をかけた
// sinc でサンプルの値を計算する。チャンネル数は作るときに決める (インターリーブ)。
//
// FFmpeg の libswresample (swr_set_compensation) を使わないのは、次の 2 つのためである。(1) 変換の内部にためる
// サンプルの数が呼び出しごとに変わり、書き出したサンプルが入力のどの位置に当たるかを、ここで決めた位置と 1 サンプルの
// 単位で突き合わせる必要がある。(2) 配信と録画の両方の単体テストで、時刻の追従を FFmpeg なしで確かめたい。
class AudioDriftCorrector {
public:
	explicit AudioDriftCorrector(const AudioDriftConfig &config = AudioDriftConfig{}, std::size_t channels = 2)
		: tracker_(config), channels_(channels > 0 ? channels : 1)
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
			history_.clear();
			history_start_ = 0;
		}
		history_.insert(history_.end(), samples, samples + frames * channels_);
		render(step.frames, out);
		return step;
	}

	void reset() { tracker_.reset(); }

	const AudioDriftTracker &tracker() const { return tracker_; }
	const AudioDriftStats &stats() const { return tracker_.stats(); }

private:
	// 入力の i 番目のサンプル (チャンネル c)。受け取る前と、置き直す前の番号 (負) は無音にする。
	float input_at(std::int64_t i, std::size_t c) const
	{
		if (i < history_start_)
			return 0.0f;
		const std::size_t index = std::size_t(i - history_start_) * channels_ + c;
		return index < history_.size() ? history_[index] : 0.0f;
	}

	void render(std::size_t count, std::vector<float> *out)
	{
		out->assign(count * channels_, 0.0f);
		const std::vector<float> &kernel = detail::audio_drift_kernel();
		const std::int64_t i0 = tracker_.first_input_int();
		const double f0 = tracker_.first_input_frac();
		const double inc = tracker_.input_step();
		float coef[detail::kAudioDriftTaps];
		std::int64_t i = i0;
		for (std::size_t j = 0; j < count; ++j) {
			const double pos = f0 + double(j) * inc;
			const double whole = std::floor(pos);
			i = i0 + std::int64_t(whole);
			const double f = pos - whole;
			if (f == 0.0 && inc == 1.0) {
				for (std::size_t c = 0; c < channels_; ++c)
					(*out)[j * channels_ + c] = input_at(i, c);
			} else {
				const double p = f * double(detail::kAudioDriftPhases);
				int pi = int(p);
				if (pi >= detail::kAudioDriftPhases)
					pi = detail::kAudioDriftPhases - 1;
				const float pf = float(p - double(pi));
				const float *row0 = &kernel[std::size_t(pi) * detail::kAudioDriftTaps];
				const float *row1 = row0 + detail::kAudioDriftTaps;
				for (int k = 0; k < detail::kAudioDriftTaps; ++k)
					coef[k] = row0[k] + (row1[k] - row0[k]) * pf;
				const std::int64_t first = i - (kAudioDriftHalfTaps - 1);
				const bool inside = first >= history_start_ &&
						    std::size_t(first - history_start_ + detail::kAudioDriftTaps) * channels_ <=
							    history_.size();
				for (std::size_t c = 0; c < channels_; ++c) {
					float acc = 0.0f;
					if (inside) {
						const std::size_t at = std::size_t(first - history_start_) * channels_ + c;
						const float *src = &history_[at];
						for (int k = 0; k < detail::kAudioDriftTaps; ++k)
							acc += coef[k] * src[std::size_t(k) * channels_];
					} else {
						for (int k = 0; k < detail::kAudioDriftTaps; ++k)
							acc += coef[k] * input_at(first + k, c);
					}
					(*out)[j * channels_ + c] = acc;
				}
			}
		}
		// 次の計算に要らない古いサンプルを捨てる (次に書き出す位置の前 kAudioDriftHalfTaps サンプルは残す)。
		i = tracker_.next_read_int();
		const std::int64_t keep_from = i - kAudioDriftHalfTaps;
		if (keep_from > history_start_) {
			const std::size_t drop =
				std::min(std::size_t(keep_from - history_start_) * channels_, history_.size());
			history_.erase(history_.begin(), history_.begin() + std::ptrdiff_t(drop));
			history_start_ += std::int64_t(drop / channels_);
		}
	}

	AudioDriftTracker tracker_;
	std::size_t channels_;
	std::vector<float> history_;    // 受け取った入力 (インターリーブ)。history_start_ 番目から
	std::int64_t history_start_ = 0;
};

} // namespace cordmux::align
