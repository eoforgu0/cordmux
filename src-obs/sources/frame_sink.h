// SPDX-License-Identifier: Apache-2.0

// 非同期の映像のソース 1 つに、フレームを渡す、空ける、保ってから空ける予約を置く、をまとめたコンポーネント。合成のソースの子
// (composite_source.cpp) が OBS に渡すための、output_mu で守る状態と手順を 1 か所に持つ。

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

#include <obs-module.h>
#include <util/platform.h>

namespace cordmux::sources {

class FrameSink {
public:
	// 渡し先のソース。作った側が destroy までに空にする。
	obs_source_t *source = nullptr;

	// フレームを渡す。方式 (OBS の緩衝あり、なし) を替えるときは、OBS に溜まったフレームと表示の時刻の基準を捨てて
	// から替える。緩衝ありのまま溜めたフレームを緩衝なしに替えても最新の 1 つが残るだけだが、緩衝なしから緩衝ありに
	// 替えると、OBS は最後に見たフレームのタイムスタンプを基準に表示の時刻を決めるので、D の分だけ先のフレームを
	// 待つことになる。NULL を渡すと、その基準 (last_frame_ts) と溜めたフレームを OBS が捨てる。すぐ後に、新しい方式の
	// 最初のフレームを渡す (同じロックの中なので、間に別のフレームは入らない)。保ってから空ける予約があれば取り消す。
	void output(const obs_source_frame2 *frame, bool want_unbuffered)
	{
		std::lock_guard<std::mutex> lock(output_mu_);
		if (!source)
			return;
		if (unbuffered_ != want_unbuffered) {
			obs_source_output_video(source, nullptr);
			obs_source_set_async_unbuffered(source, want_unbuffered);
			unbuffered_ = want_unbuffered;
		}
		obs_source_output_video2(source, frame);
		last_frame_ns_.store(os_gettime_ns());
		has_frame_.store(true);
		cleared_.store(false);
		clear_deadline_ns_.store(0);
	}

	// すぐ空ける (透明にする)。予約も取り消す。
	void clear_now()
	{
		std::lock_guard<std::mutex> lock(output_mu_);
		if (!source)
			return;
		clear_deadline_ns_.store(0);
		if (cleared_.load())
			return;
		obs_source_output_video(source, nullptr);
		cleared_.store(true);
	}

	// hold_ns だけ待ってから空ける予約を置く。既に空いていれば何もしない。期限は 0 にしない (0 は予約なしを示す)。
	void hold_then_clear(std::uint64_t hold_ns)
	{
		std::lock_guard<std::mutex> lock(output_mu_);
		if (!source || cleared_.load())
			return;
		clear_deadline_ns_.store(std::max<std::uint64_t>(1, os_gettime_ns() + hold_ns));
	}

	// OBS の tick のコールバックから呼ぶ。予約の期限が来ていれば空ける。予約がないか既に空いていれば、ロックを取らずに
	// 先に戻る (フレームごとに通る処理のため)。output_mu は待たずに試す。ほかのスレッドが持っているのはフレームを
	// 出力している最中で、そのフレームが予約を取り消すので、この周期は何もしなくてよい (フレームをコピーする間、
	// 描画のスレッドを止めない)。
	void tick()
	{
		if (clear_deadline_ns_.load() == 0 || cleared_.load())
			return;

		std::unique_lock<std::mutex> lock(output_mu_, std::try_to_lock);
		if (!lock.owns_lock() || !source)
			return;
		// ロックの中で読み直す (ロックを取るまでの間にフレームが来て予約が消えていれば、空けない)。
		const std::uint64_t deadline = clear_deadline_ns_.load();
		if (deadline == 0 || cleared_.load() || os_gettime_ns() < deadline)
			return;

		// 枠は obs_source_output_video(NULL) で空ける。フレームを obs_source_output_video2 で出力していても、
		// NULL を渡すのはこの関数でよい (どちらも同じ非同期の映像の状態を変えるため)。
		clear_deadline_ns_.store(0);
		obs_source_output_video(source, nullptr);
		cleared_.store(true);
	}

	// 渡し先を外す。以後の output、clear_now、hold_then_clear、tick は何もしない。OBS の映像の関数を呼び終えてから
	// 戻る (output_mu を取る)。
	void detach()
	{
		std::lock_guard<std::mutex> lock(output_mu_);
		source = nullptr;
	}

private:
	// 「フレームを出力する、または NULL を出力して空ける」処理と、cleared_ と clear_deadline_ns_ の書き換えを、1 つの
	// 操作にまとめるロック。ロックがないと、(1) コールバックが予約を取り消した直後に出力されたフレームを、続く NULL が
	// 消す、(2) コールバックが NULL を出力した後に出力されたフレームの cleared_ = false を、コールバックの
	// cleared_ = true が上書きし、その後の clear_now が枠を空けない、の 2 つが起きる。持ったまま呼ぶのは OBS の
	// 映像の関数 (obs_source_output_video と obs_source_output_video2) だけで、cordmux のほかのロックは取らない。
	std::mutex output_mu_;

	// 渡し先を OBS の緩衝なし (async_unbuffered) にしてあるか。output_mu_ の中でだけ読み書きする。ソースを作った
	// 直後は OBS の既定の緩衝ありで、false と一致する。
	bool unbuffered_ = false;

	// 最後のフレームの時刻と、空ける予約。clear_deadline_ns_ が 0 なら予約なし。フレームが来ないだけでは空けない。
	std::atomic<std::uint64_t> last_frame_ns_{0};
	std::atomic<bool> has_frame_{false};
	std::atomic<bool> cleared_{true};
	std::atomic<std::uint64_t> clear_deadline_ns_{0};
};

} // namespace cordmux::sources
