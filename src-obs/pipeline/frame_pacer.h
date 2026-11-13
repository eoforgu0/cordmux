// SPDX-License-Identifier: Apache-2.0

// 画面共有のデコードしたフレームを、「送信側のタイムスタンプ + 表示の緩衝 D」の時刻まで保持し、時刻が来たフレームを
// OBS に渡す順に取り出す待ち行列 (FramePacer)。届き方の揺れを D で吸い、フレームの表示の時刻を OBS ではなく
// cordmux が決める。時刻はすべて呼び出し元が渡し、ここでは時計を読まないので、単体テストで固定の時刻を与えて動作を
// 確認できる。取り出すスレッドと OBS への受け渡しは pipeline.cpp にある。時計は呼び出し元が決める (pipeline.cpp は
// OBS の描く刻みと同じ os_gettime_ns の時計にそろえる)。
// libobs にも asio にも依存しない。ロックは持たず、呼び出し元のロックの中で使う。

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace cordmux::pipeline {

// 待ち行列に持てるフレームの数の上限を、D とフレームの間隔から計算する。D の間に届くフレームの数に、取り出す
// スレッドが少し遅れても定常状態では捨てない余裕を足した数で、D が長いほど持つ数が増えるので、固定の値にしない。
//   delay_us          表示の緩衝 D (マイクロ秒)
//   frame_interval_us 送信側のフレームの間隔の見込み (マイクロ秒。60 fps なら約 16667)
//   headroom_frames   余裕のフレーム数
// frame_interval_us が 0 以下か D が 0 以下なら、余裕の分だけを上限にする。戻り値は 1 以上。
inline std::size_t frame_pacer_capacity(std::int64_t delay_us, std::int64_t frame_interval_us,
					std::size_t headroom_frames)
{
	const std::size_t in_flight = (frame_interval_us > 0 && delay_us > 0)
					      ? static_cast<std::size_t>((delay_us + frame_interval_us - 1) / frame_interval_us)
					      : 0;
	const std::size_t total = in_flight + headroom_frames;
	return total > 0 ? total : 1;
}

// 送信側のタイムスタンプが、積んだ時点の受信側の今より先にあってよい長さの上限 (マイクロ秒)。送信側は、映像の
// タイムスタンプが WebRTC の描く予定の時刻なら、そのまま送る (src-vencord/encode/video_time.ts)。WebRTC はデコードを
// 終えたフレームを描く予定の時刻より前に渡すので、フレームは自分のタイムスタンプより前に届く (age_us が負)。
// テスト用の送信側の試しで、描く予定の時刻は読んだ時刻より中央値で 13.9 ms 先にあり、受信を始めた直後には、
// 描く予定の時刻が届く時刻より 100 ms 以上先にあることがある (docs/architecture.md の「送信側: 送信側の時計への換算」)。
// その先の時刻まで待たないと、映像はタイムスタンプ + D より早く出て、同じタイムスタンプで置く画面の音とずれる。
// 上限は、受信を始めた直後の先の長さより十分大きく、待ち行列に持つフレームの数を抑える値にした。目安の値で、測って
// 決めたものではない。これより先のタイムスタンプは、時計の飛び (送信側の時計が受信側より進んだ) とみなし、この長さ
// だけ先にあるものとして扱う。
inline constexpr std::int64_t kFramePacerMaxAheadUs = 500000;

// 統計の 1 周期分の数 (FramePacer::take_stats)。
struct FramePacerStats {
	std::uint64_t on_time = 0; // 時刻が来るまで待ってから渡した数
	std::uint64_t late = 0;    // 積んだ時点でもう表示の時刻 r を過ぎていて、すぐ渡した数 (格子の有無に依らない)
	std::uint64_t dropped = 0; // 渡さずに捨てた数 (同時に時刻が来た古いフレーム、上限超過、破棄)
	std::size_t max_queued = 0; // 周期の間の待ち行列の長さの最大
	// 周期の間の、タイムスタンプが積んだ時点の今より先にあった長さの最大 (マイクロ秒。先になければ 0)。
	std::int64_t max_ahead_us = 0;
	// 先の長さが kFramePacerMaxAheadUs を超え、その長さに頭打ちにした数。
	std::uint64_t ahead_capped = 0;
};

// FramePacer::take の結果。
template <typename T> struct PacedTake {
	std::optional<T> item;   // 渡すフレーム。時刻が来たものがなければ値を持たない
	bool late = false;       // item が、積んだ時点でもう時刻を過ぎていたか
	std::size_t skipped = 0; // item より古く、渡さずに捨てたフレームの数
};

// 時刻の決め方。フレーム 1 つの表示の時刻 r は「送信側のタイムスタンプを受信側の単調な時計に置き換えた値 + D」である。
// 置き換えは積むときに 1 回だけ行う (push の age_us)。呼び出し元 (pipeline.cpp) は、age_us を OBS の時計と、接続ごとに
// 決めた送信側の時計との差 (pipeline/sender_clock.h) から作る。
//
// 刻みの格子。OBS は映像を決まった間隔の刻みで描き、刻みの直後に、非同期のソースが表示するフレームを選ぶ。渡す時刻を
// r のままにすると、渡す時刻の列 (送信側の間隔) と刻みの列が同じ間隔で位相が近いときに、起きる時刻の 1 ms 程度の
// 揺れで、1 つの刻みに 2 フレーム (古いほうは表示されない) と、次の刻みに 0 フレーム (前のフレームがもう 1 回出る)
// が繰り返される。そこで set_tick_grid で刻みの格子 (ある刻みの時刻と間隔) を教えると、r 以後の最初の刻みを
// tick(r) として、その半刻み前 (tick(r) - 間隔 / 2) を渡す時刻にする。渡す側と描く側の起きる時刻の揺れの合計が
// 半刻み未満なら、フレームは必ず tick(r) の刻みに表示される。同じ刻みに複数のフレームが入るのは、送信側の間隔が
// 刻みより本当に短いとき (60 fps の共有を 30 fps で描くなど) だけで、そのときは下の約束で新しいほうだけを渡す。
// 格子の間隔が 0 以下なら格子はなく、渡す時刻は r のままである。渡す時刻は積むときに固定せず、取り出しと
// next_release_us のたびに現在の格子と D で決めるので、格子や D が替わっても積んであるフレームに適用される。
//
// 並びの約束。フレームは積んだ順 (送信側のタイムスタンプの順) に表示する。時刻が来たフレームが複数あれば、最も新しい
// 1 つだけを渡し、それより古いフレームは渡さずに捨てる。OBS の緩衝なしの非同期ソースも、描画のたびに最後の 1 枚
// だけを表示する (libobs の ready_async_frame の async_unbuffered の枝) ので、古いフレームを渡しても表示されず、
// OBS の中へのコピー (1080p の NV12 で約 3 MB) が無駄になるだけである。
//
// 時計の飛びへの備え。待つ長さは積んだ時点から最長でも D + kFramePacerMaxAheadUs である (age_us を
// -kFramePacerMaxAheadUs 以上に丸めるため)。送信側の時計が受信側より大きく進んでいても、フレームがそれより長く
// 止まったり、待ち行列が上限まで伸びたりしない。遅れて届いたフレームは age_us が D 以上になり、すぐ渡す側に倒れる。
template <typename T> class FramePacer {
public:
	FramePacer() = default;

	// delay_us が 0 未満なら 0 にする。max_items と max_bytes が 0 のときは 1 として扱う。呼び出し元の計算を
	// 誤ったときに、積んだフレームをすぐ全部捨てる待ち行列にしないためである。
	FramePacer(std::int64_t delay_us, std::size_t max_items, std::size_t max_bytes)
	{
		configure(delay_us, max_items, max_bytes);
	}

	// D と上限を替える。積んであるフレームの時刻も、新しい D で決め直す (D を縮めると、すぐ渡す側に倒れて
	// 溜まりが解ける。伸ばすと、積んであるフレームもその分だけ待つ)。上限が小さくなっても、積んであるフレームは
	// 次の push で古いほうから捨てる。
	void configure(std::int64_t delay_us, std::size_t max_items, std::size_t max_bytes)
	{
		delay_us_ = delay_us > 0 ? delay_us : 0;
		max_items_ = max_items > 0 ? max_items : 1;
		max_bytes_ = max_bytes > 0 ? max_bytes : 1;
	}

	// 刻みの格子を設定する。
	//   phase_us     刻みのどれか 1 つの時刻 (過去でも未来でもよい。push と take の now_us と同じ時計)
	//   interval_us  刻みの間隔。0 以下なら格子をなくし、渡す時刻を r のままにする
	// 積んであるフレームの渡す時刻も、新しい格子で決め直される。
	void set_tick_grid(std::int64_t phase_us, std::int64_t interval_us)
	{
		tick_phase_us_ = phase_us;
		tick_interval_us_ = interval_us > 0 ? interval_us : 0;
	}

	// フレームを 1 つ積む。
	//   now_us    受信側の単調な時計の今 (マイクロ秒)
	//   age_us    受信側の単調な時計の今 - フレームのタイムスタンプ (整列した値を同じ時計に換算した値)。負は、
	//             タイムスタンプが今より先にある (描く予定の時刻のまま送られたフレーム)。-kFramePacerMaxAheadUs より
	//             小さければ、その値として扱う
	//   bytes     フレームの大きさ (バイト数の上限の判定に使う)
	// 上限 (数かバイト数) を超えたら、古いほうから捨てる。新しいほうを捨てると、取り出す側が詰まっている間、
	// 古い映像を出し続ける。ただし積んだフレーム自身は捨てない。捨てた数を返す。
	std::size_t push(std::int64_t now_us, std::int64_t age_us, std::size_t bytes, T item)
	{
		const std::int64_t age = age_us > -kFramePacerMaxAheadUs ? age_us : -kFramePacerMaxAheadUs;
		if (-age_us > stats_.max_ahead_us)
			stats_.max_ahead_us = -age_us;
		if (age_us < -kFramePacerMaxAheadUs)
			++stats_.ahead_capped;
		Entry entry;
		entry.sent_us = now_us - age;
		entry.bytes = bytes;
		// 遅れて届いたかどうかは、刻みに合わせる前の表示の時刻 r で決める。半刻み前に渡す時刻を過ぎただけでは数えない。
		entry.late = entry.sent_us + delay_us_ <= now_us;
		entry.item = std::move(item);
		bytes_ += bytes;
		entries_.push_back(std::move(entry));

		std::size_t dropped = 0;
		while (entries_.size() > 1 && (entries_.size() > max_items_ || bytes_ > max_bytes_)) {
			bytes_ -= entries_.front().bytes;
			entries_.pop_front();
			++dropped;
		}
		stats_.dropped += dropped;
		if (entries_.size() > stats_.max_queued)
			stats_.max_queued = entries_.size();
		return dropped;
	}

	// 次に時刻が来るフレームの時刻 (受信側の単調な時計。マイクロ秒)。空なら値を持たない。取り出すスレッドは
	// この時刻まで待つ。時刻が来ているフレームがあれば、過去の時刻を返す。
	std::optional<std::int64_t> next_release_us() const
	{
		std::optional<std::int64_t> earliest;
		for (const Entry &entry : entries_) {
			const std::int64_t release_us = release_time_us(entry.sent_us);
			if (!earliest || release_us < *earliest)
				earliest = release_us;
		}
		return earliest;
	}

	// now_us の時点で時刻が来たフレームのうち、最も新しい 1 つを取り出す。それより古いフレームは、時刻が来て
	// いなくても捨てる (新しいフレームが表示に出た後に古いフレームを渡すと、映像が逆戻りする)。時刻が来た
	// フレームがなければ何も取り出さず、待ち行列も変えない。
	PacedTake<T> take(std::int64_t now_us)
	{
		PacedTake<T> result;
		std::size_t newest_due = entries_.size();
		for (std::size_t i = 0; i < entries_.size(); ++i) {
			if (release_time_us(entries_[i].sent_us) <= now_us)
				newest_due = i;
		}
		if (newest_due == entries_.size())
			return result;

		for (std::size_t i = 0; i < newest_due; ++i) {
			bytes_ -= entries_.front().bytes;
			entries_.pop_front();
		}
		result.skipped = newest_due;
		stats_.dropped += newest_due;

		Entry &entry = entries_.front();
		result.item = std::move(entry.item);
		result.late = entry.late;
		bytes_ -= entry.bytes;
		entries_.pop_front();
		if (result.late)
			++stats_.late;
		else
			++stats_.on_time;
		return result;
	}

	// 積んであるフレームをすべて捨てて、捨てた数を返す (ストリームの終わりや停止のときに使う)。
	std::size_t discard_all()
	{
		const std::size_t count = entries_.size();
		entries_.clear();
		bytes_ = 0;
		stats_.dropped += count;
		return count;
	}

	// 統計の周期ごとに呼ぶ。数を取り出して 0 に戻す。max_queued は現在の長さから数え直す。
	FramePacerStats take_stats()
	{
		FramePacerStats result = stats_;
		stats_ = FramePacerStats{};
		stats_.max_queued = entries_.size();
		return result;
	}

	std::size_t size() const { return entries_.size(); }
	std::size_t bytes() const { return bytes_; }
	bool empty() const { return entries_.empty(); }
	std::int64_t delay_us() const { return delay_us_; }
	std::int64_t tick_interval_us() const { return tick_interval_us_; }

private:
	// 送信側の時刻 sent_us (受信側の時計の上) のフレームを渡す時刻。格子がなければ sent_us + D、あれば、その時刻
	// 以後の最初の刻み (ちょうど刻みの上ならその刻み) の半刻み前である。
	std::int64_t release_time_us(std::int64_t sent_us) const
	{
		const std::int64_t shown_us = sent_us + delay_us_;
		if (tick_interval_us_ <= 0)
			return shown_us;
		const std::int64_t diff = shown_us - tick_phase_us_;
		// 切り上げの割り算。負の diff は 0 に向かって切り捨てると切り上げになる。
		const std::int64_t ticks = diff >= 0 ? (diff + tick_interval_us_ - 1) / tick_interval_us_
						     : -((-diff) / tick_interval_us_);
		return tick_phase_us_ + ticks * tick_interval_us_ - tick_interval_us_ / 2;
	}

	struct Entry {
		std::int64_t sent_us = 0; // フレームのタイムスタンプを受信側の単調な時計に置き換えた値
		std::size_t bytes = 0;
		bool late = false;
		T item{};
	};

	std::int64_t delay_us_ = 0;
	std::int64_t tick_phase_us_ = 0;
	std::int64_t tick_interval_us_ = 0;
	std::size_t max_items_ = 1;
	std::size_t max_bytes_ = 1;
	std::size_t bytes_ = 0;
	std::deque<Entry> entries_;
	FramePacerStats stats_;
};

} // namespace cordmux::pipeline
