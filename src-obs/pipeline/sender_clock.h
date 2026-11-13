// SPDX-License-Identifier: Apache-2.0

// 送信側の時計 (timestamp_us の時計) を OBS の時計 (os_gettime_ns / 1000) に換算する差を、接続ごとに決める
// (SenderClockOffset)。どちらも Windows の単調な時計 (QueryPerformanceCounter) で進むので、差は接続の間は変わらない。
// 決め方は docs/architecture.md の「送信側の時計と OBS の時計」にある。送信側が、時計を OBS の時計と同じ値で送ると
// 伝えた (hello の clock_base が "qpc") 接続では、差を 0 にし、組は監視にだけ使う (use_shared_clock)。
// 時計は読まず、時刻はすべて引数で受け取る。libobs にも asio にも依存しない。スレッドセーフではない。

#pragma once

#include <cstdint>
#include <optional>

namespace cordmux::pipeline {

// 送信側の時計が OBS の時計と同じ値のとき (use_shared_clock)、組の値 (受け取った時の OBS の時計 - 送信側の読み) は届くまでの
// 遅れで、負にならない。ただし送信側の時計は、ページの原点の TimeTicks を求めた値の上にあり、その誤差 (上限 1 ms。
// src-vencord/encode/ticks_origin.ts の TICKS_ORIGIN_ERROR_MAX_US) の分だけ負になりうる。最小の組がこれより負なら、
// 同じ時計という前提が崩れている (呼び出し元がログに書く)。
inline constexpr std::int64_t kSharedClockToleranceUs = 1000;

// 送信側の時計が OBS の時計と同じとき、最小の組がこの値より大きいままなら、送信側の時計が OBS の時計より遅れている
// (同じ時計という前提が崩れている) とみなす (マイクロ秒)。最小の組は、最も早く届いたメッセージの届くまでの遅れに、送信側の
// ページの原点の TimeTicks の誤差 (上限 kSharedClockToleranceUs) を足した値である。同じ PC の中の WebSocket で届くまでの
// 遅れは、今までの差の見積もり (最小値) が接続の初めに数回だけ、合わせて 0.5 ms ほど下がって止まること (2026-11-13 の
// 2 回の通話のログ) から、1 ms 未満と見込む。それに誤差の上限を足しても 2 ms で、その 2 倍を超える 5 ms を境にした。
// 境を超えて見積もりに戻っても、今までの差の見積もりの動きになるだけで、映像と音声の関係は崩れない。目安の値を含む。
inline constexpr std::int64_t kSharedClockBehindUs = 5000;

// kSharedClockBehindUs で決めるのに要る組の数。hello と、5 秒ごとの ping の pong を 1 分ぶん見る。最初の数組は送信側の
// JavaScript が忙しい時に届いて遅れることがある (hello は接続の処理の途中に作る) ので、最小が下がりきるまで待つ。
inline constexpr std::uint64_t kSharedClockBehindSamples = 12;

// 同じ時計という前提の判定 (shared_clock_verdict)。
enum class SharedClockVerdict {
	Holds,        // 前提は崩れていない (まだ決められないときも含む)
	SenderAhead,  // 送信側の時計が OBS の時計より先に進んでいる (最小の組が -kSharedClockToleranceUs より小さい)
	SenderBehind, // 送信側の時計が OBS の時計より遅れている (組が十分あり、最小の組が kSharedClockBehindUs より大きい)
};

// 送信側の時計が OBS の時計と同じ (use_shared_clock) ときの、最小の組と組の数からの判定。
inline SharedClockVerdict shared_clock_verdict(std::int64_t min_sample_us, std::uint64_t samples)
{
	if (min_sample_us < -kSharedClockToleranceUs)
		return SharedClockVerdict::SenderAhead;
	if (samples >= kSharedClockBehindSamples && min_sample_us > kSharedClockBehindUs)
		return SharedClockVerdict::SenderBehind;
	return SharedClockVerdict::Holds;
}

// 送信側の時計の読みと、それを載せたメッセージを受け取った OBS の時計の組から、差 (OBS の時計 - 送信側の時計) を決める。
//
// 送信側は時計を読んでからメッセージを送るので、受け取った時の OBS の時計 - 送信側の読みは、本当の差 + 届くまでの
// 遅れ になり、本当の差より小さくならない。そこで、接続の間に受け取った組の最小値を差にする。届くまでの遅れの最も
// 小さかった組の値で、残る誤差はその遅れ (同じ PC の中の WebSocket で 1 ms 未満) だけになる。最小値は組が増えると
// 下がる向きにしか動かず、下がる量は前の最小値に含まれていた遅れの分だけである。
// ping と pong の往復の真ん中を取る方法は使わない。送信側の JavaScript が忙しいと、ping が届いてから読むまでの遅れが
// 返りの遅れより長くなり、往復の真ん中が本当の差から往復の半分までずれるためである。
// 2 つの時計の速さが本当は違うと、最小値は下がり続けるか、組の値が最小値から離れていく。その兆しは
// last_excess_us (直近の組 - 最小値) に出る。
class SenderClockOffset {
public:
	// 接続が切れたとき、新しく接続したときに呼ぶ。送信側の時計は Vesktop のレンダラーごとに原点が違うので、
	// 接続をまたいで差を持ち越さない。
	void reset()
	{
		min_sample_us_.reset();
		shared_ = false;
		samples_ = 0;
		last_excess_us_ = 0;
	}

	// 送信側の時計が OBS の時計と同じ値で来る (hello の clock_base が "qpc") ことを伝える。差を 0 にし、組は差を決めるのに
	// 使わない (最小値は、届くまでの遅れの監視の値になる)。reset で解く。
	void use_shared_clock() { shared_ = true; }

	// use_shared_clock の後か。
	bool shared_clock() const { return shared_; }

	// 同じ時計という前提が崩れたときに呼ぶ。差を、これまでの組の最小値 (今までの見積もり) に戻す。組は捨てない。
	void leave_shared_clock() { shared_ = false; }

	// 組を 1 つ足す。sender_us は送信側がメッセージを送る前に読んだ送信側の時計、obs_now_us はそのメッセージを
	// 受け取った時の OBS の時計。組の最小値が変わった (最初の組か、最小値が下がった) ときは true を返す。
	// use_shared_clock の前なら、最小値が差である。
	bool note(std::int64_t sender_us, std::int64_t obs_now_us)
	{
		const std::int64_t sample = obs_now_us - sender_us;
		++samples_;
		if (!min_sample_us_ || sample < *min_sample_us_) {
			min_sample_us_ = sample;
			last_excess_us_ = 0;
			return true;
		}
		last_excess_us_ = sample - *min_sample_us_;
		return false;
	}

	// 差 (OBS の時計 - 送信側の時計)。use_shared_clock の後は 0。その前は組の最小値で、まだ組がなければ値を持たない。
	std::optional<std::int64_t> offset_us() const
	{
		if (shared_)
			return std::int64_t{0};
		return min_sample_us_;
	}

	// 組の最小値 (受け取った時の OBS の時計 - 送信側の読み)。まだ組がなければ値を持たない。use_shared_clock の後は、
	// 最も早く届いたメッセージの届くまでの遅れで、0 以上の小さい値になるはず (kSharedClockToleranceUs)。
	std::optional<std::int64_t> min_sample_us() const { return min_sample_us_; }

	// この接続で受け取った組の数。
	std::uint64_t samples() const { return samples_; }

	// 直近の組の値 - 最小値 (us)。その組が届くまでにかかった遅れの、最も小さかった組との差である。
	std::int64_t last_excess_us() const { return last_excess_us_; }

private:
	std::optional<std::int64_t> min_sample_us_;
	bool shared_ = false;
	std::uint64_t samples_ = 0;
	std::int64_t last_excess_us_ = 0;
};

// 送信側のタイムスタンプを OBS の時計に換算する。offset_us は SenderClockOffset::offset_us() の値。差がまだ決まって
// いない (送信側が時計の読みを送ってこない) ときは、そのメッセージが今生まれたものとして obs_now_us を返す。
inline std::int64_t sender_to_obs_us(std::int64_t sender_us, std::optional<std::int64_t> offset_us,
				     std::int64_t obs_now_us)
{
	return offset_us ? sender_us + *offset_us : obs_now_us;
}

} // namespace cordmux::pipeline
