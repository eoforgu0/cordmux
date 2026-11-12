// SPDX-License-Identifier: Apache-2.0

// 送信側の時計 (timestamp_us の時計) を OBS の時計 (os_gettime_ns / 1000) に換算する差を、接続ごとに決める
// (SenderClockOffset)。どちらも Windows の単調な時計 (QueryPerformanceCounter) で進むので、差は接続の間は変わらない。
// 決め方は docs/architecture.md の「送信側の時計と OBS の時計」にある。
// 時計は読まず、時刻はすべて引数で受け取る。libobs にも asio にも依存しない。スレッドセーフではない。

#pragma once

#include <cstdint>
#include <optional>

namespace cordmux::pipeline {

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
		offset_us_.reset();
		samples_ = 0;
		last_excess_us_ = 0;
	}

	// 組を 1 つ足す。sender_us は送信側がメッセージを送る前に読んだ送信側の時計、obs_now_us はそのメッセージを
	// 受け取った時の OBS の時計。差が変わった (最初の組か、最小値が下がった) ときは true を返す。
	bool note(std::int64_t sender_us, std::int64_t obs_now_us)
	{
		const std::int64_t sample = obs_now_us - sender_us;
		++samples_;
		if (!offset_us_ || sample < *offset_us_) {
			offset_us_ = sample;
			last_excess_us_ = 0;
			return true;
		}
		last_excess_us_ = sample - *offset_us_;
		return false;
	}

	// 差 (OBS の時計 - 送信側の時計)。まだ組がなければ値を持たない。
	std::optional<std::int64_t> offset_us() const { return offset_us_; }

	// この接続で受け取った組の数。
	std::uint64_t samples() const { return samples_; }

	// 直近の組の値 - 最小値 (us)。その組が届くまでにかかった遅れの、最も小さかった組との差である。
	std::int64_t last_excess_us() const { return last_excess_us_; }

private:
	std::optional<std::int64_t> offset_us_;
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
