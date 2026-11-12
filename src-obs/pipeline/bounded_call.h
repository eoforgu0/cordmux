// SPDX-License-Identifier: Apache-2.0

// タイムアウトを付けて 1 つの問い合わせを実行するクラス。録画の保存先の候補に空き容量を問い合わせるのに使う。
// 到達できないネットワークの場所への問い合わせは、OS の側でタイムアウトするまで数十秒返らない。保存先は
// プラグインの読み込み中にも決めるので、返るまで待つと OBS の起動がその間止まる。
// libobs にも Qt にも依存しない。

#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace cordmux::pipeline {

// タイムアウト付きの問い合わせ。1 つのインスタンスは 1 本のスレッドからだけ使う (待つのをやめたスレッドの一覧を
// ロックなしで持つため)。
class BoundedCall {
public:
	BoundedCall() = default;
	~BoundedCall() { join_pending(); }
	BoundedCall(const BoundedCall &) = delete;
	BoundedCall &operator=(const BoundedCall &) = delete;

	// work を別のスレッドで実行し、timeout_ms まで待つ。返った値を返し、タイムアウトしたら値を持たない
	// (「答えがない」と「答えが false」を区別する)。timeout_ms が 0 以下なら、待たずに値のない答えを返す。
	// work が例外を投げたときは false を返す。待つのをやめたスレッドは join_pending() まで保持する。
	std::optional<bool> ask(const std::function<bool()> &work, int timeout_ms)
	{
		if (!work)
			return std::nullopt;
		// 待つのをやめた後もスレッドが答えを書き込むので、呼び出し元のスタックではなく shared_ptr で持つ。
		auto answer = std::make_shared<std::promise<bool>>();
		std::future<bool> future = answer->get_future();
		std::thread worker([answer, work]() {
			bool value = false;
			try {
				value = work();
			} catch (...) {
				value = false; // 例外は「読めなかった」と同じに扱う
			}
			answer->set_value(value);
		});

		const bool answered = timeout_ms > 0 &&
				      future.wait_for(std::chrono::milliseconds(timeout_ms)) ==
					      std::future_status::ready;
		if (answered) {
			worker.join();
			return future.get();
		}
		// タイムアウトしたので待つのをやめた。detach すると、プラグインがアンロードされた後にスレッドが戻り、
		// アンロード済みのコードを実行するので、保持して後で join する。
		pending_.push_back(std::move(worker));
		return std::nullopt;
	}

	// 待つのをやめたスレッドをすべて join する。デストラクターからも呼ばれる。join したスレッドは一覧から消える。
	void join_pending()
	{
		for (std::thread &worker : pending_) {
			if (worker.joinable())
				worker.join();
		}
		pending_.clear();
	}

	// まだ join していないスレッドの数 (テストから確認するため)。
	std::size_t pending_count() const { return pending_.size(); }

private:
	std::vector<std::thread> pending_;
};

} // namespace cordmux::pipeline
