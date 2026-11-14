// SPDX-License-Identifier: Apache-2.0

// 非同期の映像のソース 1 つに、フレームを渡す、空ける、保ってから空ける予約を置く、をまとめたコンポーネントの本体。OBS の関数は
// Port (sources/frame_sink.h の ObsFramePort) を通して呼ぶので、libobs にも Qt にも依存せず、テストが偽の Port で
// 「要求、フレーム、tick」の順を通せる。空ける (Port::output_null) のは tick の中だけで行う (sources/frame_sink.h)。

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "sources/frame_sink_clear.h"

namespace cordmux::sources {

// Port が持つもの: 型 Source と Frame、static な関数 output_frame(Source *, const Frame *)、output_null(Source *)、
// set_unbuffered(Source *, bool)、now_ns()。
template <typename Port> class BasicFrameSink {
public:
	using Source = typename Port::Source;
	using Frame = typename Port::Frame;

	// 渡し先のソース。作った側が destroy までに空にする。
	Source *source = nullptr;

	// フレームを渡す。方式 (OBS の緩衝あり、なし) を替えるときは、OBS に溜まったフレームと表示の時刻の基準を捨てて
	// から替える。緩衝ありのまま溜めたフレームを緩衝なしに替えても最新の 1 つが残るだけだが、緩衝なしから緩衝ありに
	// 替えると、OBS は最後に見たフレームのタイムスタンプを基準に表示の時刻を決めるので、D の分だけ先のフレームを
	// 待つことになる。NULL を渡すと、その基準 (last_frame_ts) と溜めたフレームを OBS が捨てる。すぐ後に、新しい方式の
	// 最初のフレームを渡す (同じロックの中なので、間に別のフレームは入らない)。この NULL だけは tick の外で渡す (方式を
	// 替えるのは映像のバッファの設定を変えたときと、子の最初のフレームのとき)。保ってから空ける予約があれば取り消す。
	void output(const Frame *frame, bool want_unbuffered)
	{
		std::lock_guard<std::mutex> lock(output_mu_);
		if (!source)
			return;
		if (unbuffered_ != want_unbuffered) {
			Port::output_null(source);
			Port::set_unbuffered(source, want_unbuffered);
			unbuffered_ = want_unbuffered;
		}
		Port::output_frame(source, frame);
		last_frame_ns_.store(Port::now_ns());
		has_frame_.store(true);
		cleared_.store(false);
		clear_deadline_ns_.store(kNoClear);
	}

	// 次の tick で空ける (透明にする)。保ってから空ける予約は、この要求に置き換える。既に空いていれば、予約を取り消すだけ。
	// 次の tick までにフレームが来れば、要求は取り消され、そのフレームを表示する (前のすぐ空ける形でも、空けた直後に来た
	// フレームは表示し直していた)。
	void clear_now()
	{
		std::lock_guard<std::mutex> lock(output_mu_);
		if (!source)
			return;
		clear_deadline_ns_.store(cleared_.load() ? kNoClear : kClearAtNextTick);
	}

	// hold_ns だけ待ってから空ける予約を置く。既に空いていれば何もしない。すぐ空ける要求が既にあれば、それを保つ
	// (frame_sink_hold_deadline)。
	void hold_then_clear(std::uint64_t hold_ns)
	{
		std::lock_guard<std::mutex> lock(output_mu_);
		if (!source || cleared_.load())
			return;
		clear_deadline_ns_.store(frame_sink_hold_deadline(clear_deadline_ns_.load(), Port::now_ns(), hold_ns));
	}

	// OBS の tick のコールバックから呼ぶ。予約の期限が来ていれば空ける (すぐ空ける要求は期限が kClearAtNextTick なので、
	// 次の tick で空ける)。予約がないか既に空いていれば、ロックを取らずに先に戻る (フレームごとに通る処理のため)。
	// output_mu は待たずに試す。ほかのスレッドが持っているのはフレームを出力している最中で、そのフレームが予約を
	// 取り消すので、この周期は何もしなくてよい (フレームをコピーする間、描画のスレッドを止めない)。
	void tick()
	{
		if (clear_deadline_ns_.load() == kNoClear || cleared_.load())
			return;

		std::unique_lock<std::mutex> lock(output_mu_, std::try_to_lock);
		if (!lock.owns_lock() || !source)
			return;
		// ロックの中で読み直す (ロックを取るまでの間にフレームが来て予約が消えていれば、空けない)。
		if (!frame_sink_clear_due(clear_deadline_ns_.load(), cleared_.load(), Port::now_ns()))
			return;

		// 枠は NULL を渡して空ける (OBS の obs_source_output_video(NULL))。フレームを obs_source_output_video2 で出力して
		// いても、NULL を渡すのはこの関数でよい (どちらも同じ非同期の映像の状態を変えるため)。
		clear_deadline_ns_.store(kNoClear);
		Port::output_null(source);
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
	// cleared_ = true が上書きし、その後の clear_now が枠を空けない、の 2 つが起きる。持ったまま呼ぶのは Port の
	// 映像の関数だけで、cordmux のほかのロックは取らない。
	std::mutex output_mu_;

	// 渡し先を OBS の緩衝なし (async_unbuffered) にしてあるか。output_mu_ の中でだけ読み書きする。ソースを作った
	// 直後は OBS の既定の緩衝ありで、false と一致する。
	bool unbuffered_ = false;

	// 最後のフレームの時刻と、空ける予約 (期限の値は frame_sink_clear.h)。フレームが来ないだけでは空けない。
	std::atomic<std::uint64_t> last_frame_ns_{0};
	std::atomic<bool> has_frame_{false};
	std::atomic<bool> cleared_{true};
	std::atomic<std::uint64_t> clear_deadline_ns_{0};
};

} // namespace cordmux::sources
