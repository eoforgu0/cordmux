// SPDX-License-Identifier: Apache-2.0

// シナリオの手順を、間隔を空けて順に実行するクラス。手順には次の 3 種類がある。
//   then           手順を実行し、終わってから次の手順を予約する。
//   then_blocking  メニュー (QMenu::exec) やダイアログ (QDialog::exec) を開いて、閉じるまで戻らない手順。
//   then_when      条件が真になるまで待ってから実行する手順。

#pragma once

#include <QApplication>
#include <QDialog>
#include <QEventLoop>
#include <QTimer>
#include <QWidget>

#include <deque>
#include <functional>
#include <utility>

namespace cordmux::dock_drive {

class Script {
public:
	// 前の手順から delay_ms の後に `step` を実行する。次の手順は、`step` が終わってから予約する。
	// 先に予約すると、`step` の中でイベントを処理したとき (scen_common.h の pump) に次の手順がその中で先に動き、
	// 順が崩れる。
	void then(int delay_ms, std::function<void()> step) { steps_.push_back({delay_ms, std::move(step), false}); }
	// then と同じだが、`step` はメニューかダイアログを開き、閉じるまで戻らない (イベントループの入れ子)。次の手順を
	// `step` の前に予約するので、次の手順は入れ子のイベントループの中で動き、メニューやダイアログを操作して
	// 閉じられる。後で予約すると、`step` から戻らないので止まる。
	void then_blocking(int delay_ms, std::function<void()> step)
	{
		steps_.push_back({delay_ms, std::move(step), true});
	}
	// 前の手順から delay_ms の後、ready が真になるまで (最長 limit_ms) 待ってから、`step` に ready の結果を渡して
	// 実行する。待つ間は手順の中にとどまらずにイベントループへ戻り、20 ms ごとに ready を確認するので、
	// 入れ子のダイアログ (保存のダイアログの exec など) は閉じて戻れる。手順の中でイベントを処理して待つと、
	// 手順が入れ子のダイアログのイベントループの中で動いているときに、そのダイアログの exec が戻れず、
	// 待っているものが来ない。決まった時間だけ待つと、負荷が高くてダイアログが開くのが遅れたときに、
	// 確認がダイアログを閉じる前に動いてしまう。
	void then_when(int delay_ms, std::function<bool()> ready, int limit_ms, std::function<void(bool)> step)
	{
		Step st{delay_ms, nullptr, false};
		st.ready = std::move(ready);
		st.limit_ms = limit_ms;
		st.fn_ready = std::move(step);
		steps_.push_back(std::move(st));
	}

	// すべての手順を実行し、終わるまでイベントを処理する。timeout_ms を超えたら、開いているメニューとダイアログを
	// 閉じて false を返す。
	bool run(int timeout_ms = 120000)
	{
		QEventLoop loop;
		loop_ = &loop;
		bool timed_out = false;
		QTimer guard;
		guard.setSingleShot(true);
		QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
			timed_out = true;
			// 開いたままのメニューとダイアログを閉じる (入れ子のイベントループが終わらないと、
			// 外のループも終われない)。
			close_nested();
			loop.quit();
		});
		guard.start(timeout_ms);
		schedule_next();
		loop.exec();
		loop_ = nullptr;
		return !timed_out && steps_.empty() && running_ == 0;
	}

	// 開いているメニューとモーダルのダイアログをすべて閉じる (タイムアウトしたときの後片付け)。
	static void close_nested()
	{
		for (int i = 0; i < 8; ++i) {
			if (QWidget *popup = QApplication::activePopupWidget()) {
				popup->close();
				continue;
			}
			if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
				modal->reject();
				continue;
			}
			break;
		}
		// モーダルでないダイアログ (親ウィンドウだけを止める OwnerOnly の形) も自分のイベントループで待つので、
		// 閉じないと外のループが終われない。
		for (QWidget *w : QApplication::topLevelWidgets()) {
			if (auto *d = qobject_cast<QDialog *>(w); d != nullptr && d->isVisible())
				d->reject();
		}
	}

private:
	struct Step {
		int delay_ms;
		std::function<void()> fn;
		bool blocking;
		std::function<bool()> ready;      // then_when だけ
		int limit_ms = 0;                 // then_when だけ
		std::function<void(bool)> fn_ready; // then_when だけ
		int waited_ms = 0;                // then_when で待った時間
	};

	void schedule_next()
	{
		if (steps_.empty()) {
			if (running_ == 0 && loop_ != nullptr)
				loop_->quit();
			return;
		}
		const int delay = steps_.front().delay_ms;
		QTimer::singleShot(delay, [this] { fire(); });
	}

	void fire()
	{
		if (steps_.empty())
			return;
		if (steps_.front().ready) {
			Step &front = steps_.front();
			const bool ok = front.ready();
			if (!ok && front.waited_ms < front.limit_ms) {
				front.waited_ms += 20;
				QTimer::singleShot(20, [this] { fire(); });
				return;
			}
			Step step = std::move(front);
			steps_.pop_front();
			++running_;
			step.fn_ready(ok);
			--running_;
			schedule_next();
			return;
		}
		Step step = std::move(steps_.front());
		steps_.pop_front();
		if (step.blocking)
			schedule_next(); // 先に次を予約してから実行する (then_blocking のコメント)
		++running_;
		step.fn();
		--running_;
		if (!step.blocking)
			schedule_next();
		else if (steps_.empty() && running_ == 0 && loop_ != nullptr)
			loop_->quit();
	}

	std::deque<Step> steps_;
	QEventLoop *loop_ = nullptr;
	int running_ = 0;
};

} // namespace cordmux::dock_drive
