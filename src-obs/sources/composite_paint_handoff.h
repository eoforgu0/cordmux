// SPDX-License-Identifier: Apache-2.0

// 合成のソース cordmux の名前ラベルと光の画像を、描画のスレッドの外で描くための、仕事の受け渡し (PaintHandoff)。
// libobs にも Qt にも依存しないので、テスト (align_tests) がヘッダーを直接読む。描く関数は呼び出し元が渡す。
//
// 流れ。描画のスレッドが submit で仕事 (鍵と入力) を置き、描き直しのスレッドが paint を呼んで画像を作り、描き終えた画像を
// 保存先に置く。描画のスレッドは take で、いま欲しい鍵と同じ画像だけを受け取る。仕事は「保存先の番号」(slot) ごとに
// 1 つだけ待たせ、同じ保存先に新しい仕事が来たら、古い未着手の仕事は捨てる。
//
// 同じ鍵の仕事は、待っているか、描いている最中か、描き終えて受け取り待ちの間は、もう一度置いても受け付けない。
// 鍵が欲しいものと違う描き終えた画像は、take が捨てる。
//
// ロックは mu_ の 1 つだけで、これは葉のロック (持ったまま、ほかのロックを取らず、paint も呼ばない)。paint は
// ロックを放して呼ぶ。描き直しのスレッドは、paint 以外のことをしない。描き終えた画像 (Output) を捨てるときも、ロックを
// 放してから壊す。Output が壊れるときにほかのロック (OBS のグラフィックスのコンテキストなど) を取る型でもよいように
// するためである。

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace cordmux::sources {

// 受け渡しの計数。take_counts が読んで 0 に戻す (周期ごとの値)。
struct PaintHandoffCounts {
	std::uint64_t submitted = 0;      // 受け付けた仕事の数
	std::uint64_t finished = 0;       // 描き終えた数
	std::uint64_t dropped = 0;        // 描かずに捨てた仕事と、受け取らずに捨てた画像の数
	std::int64_t max_paint_ns = 0;    // 1 回の描画にかかった時間の最大
};

template <typename Key, typename Input, typename Output>
class PaintHandoff {
public:
	using PaintFn = std::function<Output(const Input &)>;

	struct Done {
		Key key;
		Output output;
		std::int64_t paint_ns = 0;
	};

	// slot_count は保存先の数 (add_slot で後から増やせる)。start_thread が偽なら描き直しのスレッドを作らず、テストが
	// run_one で 1 つずつ描く。
	PaintHandoff(std::size_t slot_count, PaintFn paint, bool start_thread = true)
		: paint_(std::move(paint)), slots_(slot_count)
	{
		if (start_thread)
			thread_ = std::thread([this] { loop(); });
	}

	~PaintHandoff() { stop(); }

	PaintHandoff(const PaintHandoff &) = delete;
	PaintHandoff &operator=(const PaintHandoff &) = delete;

	// 保存先を 1 つ足し、その番号を返す。番号は足した順で、減らさない (使わなくなった保存先は呼び出し元が覚えて使い回す)。
	// 描き直しのスレッドが描いている最中に呼んでもよい。
	std::size_t add_slot()
	{
		std::lock_guard<std::mutex> lock(mu_);
		slots_.emplace_back();
		return slots_.size() - 1;
	}

	// 保存先 slot に鍵 key の仕事を置く。入力は受け付けるときだけ make_input で作る。同じ鍵の仕事がすでにあるとき、
	// 止めた後、保存先の番号が範囲の外のときは受け付けず false を返す。
	template <typename MakeInput>
	bool submit(std::size_t slot, const Key &key, MakeInput &&make_input)
	{
		{
			std::lock_guard<std::mutex> lock(mu_);
			if (stopped_ || slot >= slots_.size())
				return false;
			Slot &s = slots_[slot];
			if ((s.pending && s.pending->key == key) || (s.running && *s.running == key) ||
			    (s.done && s.done->key == key))
				return false;
			if (s.pending)
				++counts_.dropped;
			s.pending = Job{key, make_input(), ++sequence_};
			++counts_.submitted;
		}
		cv_.notify_one();
		return true;
	}

	// 保存先 slot の描き終えた画像を、鍵が wanted と同じときだけ受け取る。違う鍵の画像は、ロックを放してから捨てる。
	std::optional<Done> take(std::size_t slot, const Key &wanted)
	{
		std::optional<Done> discarded; // ロックより後に壊れるよう、先に宣言する
		std::lock_guard<std::mutex> lock(mu_);
		if (slot >= slots_.size() || !slots_[slot].done)
			return std::nullopt;
		Slot &s = slots_[slot];
		if (!(s.done->key == wanted)) {
			discarded = std::move(s.done);
			s.done.reset();
			++counts_.dropped;
			return std::nullopt;
		}
		std::optional<Done> result = std::move(s.done);
		s.done.reset();
		return result;
	}

	// 保存先 slot の待っている仕事を捨て、描き終えた画像があれば受け取らずに返す (呼び出し元が捨てる)。描いている最中の
	// 仕事は止めず、描き終えたら保存先に置かれる (次に take したときに、鍵が違えば捨てる)。使わなくなった保存先を空けるときに呼ぶ。
	std::optional<Done> clear_slot(std::size_t slot)
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (slot >= slots_.size())
			return std::nullopt;
		Slot &s = slots_[slot];
		if (s.pending) {
			s.pending.reset();
			++counts_.dropped;
		}
		std::optional<Done> result = std::move(s.done);
		if (result)
			++counts_.dropped;
		s.done.reset();
		return result;
	}

	// 全部の保存先の描き終えた画像を、鍵によらず受け取らずに返す (呼び出し元が捨てる)。止めた後に、残った画像を捨てる
	// ために呼ぶ。
	std::vector<Done> drain_done()
	{
		std::vector<Done> result;
		std::lock_guard<std::mutex> lock(mu_);
		for (Slot &s : slots_) {
			if (s.done) {
				result.push_back(std::move(*s.done));
				s.done.reset();
			}
		}
		return result;
	}

	// 待っている仕事のうち一番古いものを 1 つ描く。描いたら true。描き直しのスレッドと、テストが使う。
	bool run_one()
	{
		std::size_t index = 0;
		Job job;
		{
			std::lock_guard<std::mutex> lock(mu_);
			if (!oldest_pending_locked(&index))
				return false;
			Slot &slot = slots_[index];
			job = std::move(*slot.pending);
			slot.pending.reset();
			slot.running = job.key;
		}
		// 保存先は番号で持つ。描いている間に add_slot が保存先を足すと、保存先の場所が動くためである。
		std::optional<Output> output;
		const auto started = std::chrono::steady_clock::now();
		try {
			output = paint_(job.input);
		} catch (const std::exception &) {
			output.reset();
		}
		const std::int64_t took = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
		std::optional<Done> replaced; // ロックより後に壊れるよう、先に宣言する
		{
			std::lock_guard<std::mutex> lock(mu_);
			Slot &slot = slots_[index];
			slot.running.reset();
			if (!output || stopped_) {
				++counts_.dropped;
			} else {
				if (slot.done) {
					replaced = std::move(slot.done);
					++counts_.dropped;
				}
				slot.done = Done{std::move(job.key), std::move(*output), took};
				output.reset();
				++counts_.finished;
				counts_.max_paint_ns = std::max(counts_.max_paint_ns, took);
			}
		}
		// 捨てる output と replaced は、ここ (ロックの外) で壊れる。
		return true;
	}

	// 待っている仕事を捨て、描き直しのスレッドを止めて待つ。描いている最中の仕事は、描き終えるまで待つ (その画像は捨てる)。
	// 何度呼んでもよい。描き終えて受け取り待ちの画像は残る (drain_done で受け取る)。
	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(mu_);
			stopped_ = true;
			for (Slot &s : slots_) {
				if (s.pending) {
					s.pending.reset();
					++counts_.dropped;
				}
			}
		}
		cv_.notify_all();
		if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
			thread_.join();
	}

	bool stopped() const
	{
		std::lock_guard<std::mutex> lock(mu_);
		return stopped_;
	}

	PaintHandoffCounts take_counts()
	{
		std::lock_guard<std::mutex> lock(mu_);
		const PaintHandoffCounts result = counts_;
		counts_ = PaintHandoffCounts();
		return result;
	}

private:
	struct Job {
		Key key{};
		Input input{};
		std::uint64_t sequence = 0;
	};
	struct Slot {
		std::optional<Job> pending;
		std::optional<Key> running;
		std::optional<Done> done;
	};

	// 待っている仕事のうち一番古いものの保存先の番号。なければ false。
	bool oldest_pending_locked(std::size_t *index) const
	{
		bool found = false;
		for (std::size_t i = 0; i < slots_.size(); ++i) {
			const Slot &s = slots_[i];
			if (s.pending && (!found || s.pending->sequence < slots_[*index].pending->sequence)) {
				*index = i;
				found = true;
			}
		}
		return found;
	}

	void loop()
	{
		for (;;) {
			{
				std::unique_lock<std::mutex> lock(mu_);
				std::size_t index = 0;
				cv_.wait(lock, [this, &index] { return stopped_ || oldest_pending_locked(&index); });
				if (stopped_)
					return;
			}
			run_one();
		}
	}

	PaintFn paint_;
	mutable std::mutex mu_;
	std::condition_variable cv_;
	std::vector<Slot> slots_;
	std::uint64_t sequence_ = 0;
	bool stopped_ = false;
	PaintHandoffCounts counts_;
	std::thread thread_;
};

} // namespace cordmux::sources
